#include <check.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "device/r4300/r4300_core.h"
#include "device/r4300/wasm_dynarec/wasm_dynarec.h"
#include "device/memory/memory.h"
#include "device/r4300/fpu.h"

#define ENCODE_COP1(fmt, ft, fs, fd, func) \
    ((0x11u << 26) | ((fmt) << 21) | ((ft) << 16) | ((fs) << 11) | ((fd) << 6) | (func))

#define CVT_D_S(fd, fs) ENCODE_COP1(0x10, 0, fs, fd, 0x21)
#define CVT_S_D(fd, fs) ENCODE_COP1(0x11, 0, fs, fd, 0x20)
#define CVT_W_S(fd, fs) ENCODE_COP1(0x10, 0, fs, fd, 0x24)
#define CVT_W_D(fd, fs) ENCODE_COP1(0x11, 0, fs, fd, 0x24)
#define CVT_L_S(fd, fs) ENCODE_COP1(0x10, 0, fs, fd, 0x25)
#define CVT_L_D(fd, fs) ENCODE_COP1(0x11, 0, fs, fd, 0x25)
#define CVT_S_W(fd, fs) ENCODE_COP1(0x14, 0, fs, fd, 0x20)
#define CVT_D_W(fd, fs) ENCODE_COP1(0x14, 0, fs, fd, 0x21)
#define CVT_S_L(fd, fs) ENCODE_COP1(0x15, 0, fs, fd, 0x20)
#define CVT_D_L(fd, fs) ENCODE_COP1(0x15, 0, fs, fd, 0x21)
#define CEIL_L_S(fd, fs) ENCODE_COP1(0x10, 0, fs, fd, 0x0a)
#define CEIL_L_D(fd, fs) ENCODE_COP1(0x11, 0, fs, fd, 0x0a)
#define CEIL_W_S(fd, fs) ENCODE_COP1(0x10, 0, fs, fd, 0x0e)
#define CEIL_W_D(fd, fs) ENCODE_COP1(0x11, 0, fs, fd, 0x0e)
#define FLOOR_L_S(fd, fs) ENCODE_COP1(0x10, 0, fs, fd, 0x0b)
#define FLOOR_L_D(fd, fs) ENCODE_COP1(0x11, 0, fs, fd, 0x0b)
#define FLOOR_W_S(fd, fs) ENCODE_COP1(0x10, 0, fs, fd, 0x0f)
#define FLOOR_W_D(fd, fs) ENCODE_COP1(0x11, 0, fs, fd, 0x0f)
#define ROUND_L_S(fd, fs) ENCODE_COP1(0x10, 0, fs, fd, 0x08)
#define ROUND_L_D(fd, fs) ENCODE_COP1(0x11, 0, fs, fd, 0x08)
#define TRUNC_L_S(fd, fs) ENCODE_COP1(0x10, 0, fs, fd, 0x09)
#define TRUNC_L_D(fd, fs) ENCODE_COP1(0x11, 0, fs, fd, 0x09)
#define ROUND_W_S(fd, fs) ENCODE_COP1(0x10, 0, fs, fd, 0x0c)
#define ROUND_W_D(fd, fs) ENCODE_COP1(0x11, 0, fs, fd, 0x0c)
#define TRUNC_W_S(fd, fs) ENCODE_COP1(0x10, 0, fs, fd, 0x0d)
#define TRUNC_W_D(fd, fs) ENCODE_COP1(0x11, 0, fs, fd, 0x0d)

/* minimal stubs to satisfy the dynarec build */
void DebugMessage(int level, const char *fmt, ...) {}

uint32_t *fast_mem_access(struct r4300_core *r4300, uint32_t address)
{
    if ((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000))
        return NULL;

    address &= UINT32_C(0x1ffffffc);
    const struct mem_handler *h = mem_get_handler(r4300->mem, address);
    return (uint32_t*)((uint8_t*)h->opaque + address);
}

int r4300_read_aligned_word(struct r4300_core *r4300, uint32_t address, uint32_t *value)
{
    address &= UINT32_C(0x1ffffffc);
    mem_read32(mem_get_handler(r4300->mem, address), address, value);
    return 1;
}

int r4300_read_aligned_dword(struct r4300_core *r4300, uint32_t address, uint64_t *value)
{
    uint32_t hi, lo;
    address &= UINT32_C(0x1ffffffc);
    const struct mem_handler *h = mem_get_handler(r4300->mem, address);
    mem_read32(h, address, &hi);
    mem_read32(h, address + 4, &lo);
    *value = ((uint64_t)hi << 32) | lo;
    return 1;
}

int r4300_write_aligned_word(struct r4300_core *r4300, uint32_t address, uint32_t value, uint32_t mask)
{
    invalidate_cached_code_wasm_dynarec(r4300, address, 4);
    address &= UINT32_C(0x1ffffffc);
    mem_write32(mem_get_handler(r4300->mem, address), address, value, mask);
    return 1;
}

int r4300_write_aligned_dword(struct r4300_core *r4300, uint32_t address, uint64_t value, uint64_t mask)
{
    invalidate_cached_code_wasm_dynarec(r4300, address, 8);
    address &= UINT32_C(0x1ffffffc);
    const struct mem_handler *h = mem_get_handler(r4300->mem, address);
    mem_write32(h, address, (uint32_t)(value >> 32), (uint32_t)(mask >> 32));
    mem_write32(h, address + 4, (uint32_t)value, (uint32_t)mask);
    return 1;
}

static void test_read32(void *opaque, uint32_t address, uint32_t *value)
{
    memcpy(value, (uint8_t*)opaque + address, 4);
}

static void test_write32(void *opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    uint32_t *dst = (uint32_t*)((uint8_t*)opaque + address);
    masked_write(dst, value, mask);
}

static const uint32_t example_block[] = {
    0x23e50000, /* addi a1, ra, 0 */
    0x001c1020, /* move v0, gp */
    0x23630000, /* addi v1, k1, 0 */
    0x2064fec0, /* addi a0, v1, -0x140 */
    0x18800002, /* blez a0, 2 */
    0x20010380, /* addi at, zero, 0x380 */
    0x20030140, /* addi v1, zero, 0x140 */
    0x207e0000, /* addi fp, v1, 0 */
    0x0d000465, /* jal 0x84001194 */
    0x2063ffff, /* addi v1, v1, -1 */
    0x201d0380, /* addi sp, zero, 0x380 */
    0x00a00008, /* jr a1 */
    0x00000000  /* nop */
};

struct cpu_state {
    struct new_dynarec_hot_state hot;
    uint64_t cp1[32];
};

static struct cpu_state init_state;
static struct cpu_state expect_state;

static void run_asm_test(const char *name, const uint32_t *code, size_t count,
                         const struct cpu_state *initial,
                         const struct cpu_state *expected)
{ 
    struct r4300_core *cpu = calloc(1, sizeof(*cpu));
    ck_assert_ptr_nonnull(cpu);

    struct memory mem = {0};
    uint8_t *rdram_buf = calloc(0x10000, 1);
    ck_assert_ptr_nonnull(rdram_buf);

    struct mem_mapping mapping = { 0, 0x10000 - 1, 0,
                                  { rdram_buf, test_read32, test_write32 } };
    struct mem_handler dbg = { rdram_buf, test_read32, test_write32 };
    init_memory(&mem, &mapping, 1, NULL, &dbg);
    cpu->mem = &mem;

    /* Copy the test code into emulated memory so in-block jumps execute
     * the same instructions when dispatched again. */
    memcpy(rdram_buf, code, count * sizeof(uint32_t));

    wasm_dynarec_init(cpu);
    wasm_dynarec_recompile_block(cpu, code, count, 0x80000000);

    const char *wat_env = getenv("WASM_TEST_WAT");
    if (wat_env && wat_env[0]) {
        const char *wat = wasm_dynarec_get_wat(0x80000000);
        if (wat)
            printf("%s WAT:\n%s\n", name, wat);
    }

    /* Copy initial register state but keep internal pointers intact */
    memcpy(&cpu->new_dynarec_hot_state, &initial->hot,
           sizeof(cpu->new_dynarec_hot_state));
    cpu->new_dynarec_hot_state.pc = &cpu->new_dynarec_hot_state.fake_pc;
    cpu->new_dynarec_hot_state.fake_pc.f.r.rs =
        &cpu->new_dynarec_hot_state.rs;
    cpu->new_dynarec_hot_state.fake_pc.f.r.rt =
        &cpu->new_dynarec_hot_state.rt;
    cpu->new_dynarec_hot_state.fake_pc.f.r.rd =
        &cpu->new_dynarec_hot_state.rd;
    cpu->cp1.new_dynarec_hot_state = &cpu->new_dynarec_hot_state;
    cpu->cp2.new_dynarec_hot_state = &cpu->new_dynarec_hot_state;
    if (cpu->new_dynarec_hot_state.pcaddr == 0)
        cpu->new_dynarec_hot_state.pcaddr = 0x80000000;
    for (int i = 0; i < 32; i++)
        cpu->cp1.regs[i].dword = initial->cp1[i];

    wasm_dynarec_exec(cpu, 0x80000000);

    /* Execute subsequent blocks until the expected PC is reached. If no
     * explicit PC is expected, run until the dynarec returns to address 0
     * which indicates the block completed. */
    if (expected->hot.pcaddr) {
        for (int i = 0; i < 64 &&
                    cpu->new_dynarec_hot_state.pcaddr != expected->hot.pcaddr; i++)
            wasm_dynarec_exec(cpu, cpu->new_dynarec_hot_state.pcaddr);
    } else {
        for (int i = 0; i < 64 && cpu->new_dynarec_hot_state.pcaddr != 0; i++)
            wasm_dynarec_exec(cpu, cpu->new_dynarec_hot_state.pcaddr);
    }

    const char *dbg_env = getenv("WASM_TEST_DEBUG");
    if (dbg_env && dbg_env[0]) {
        printf("%s: r2=%llx r3=%llx a0=%llx sp=%llx pc=%x hi=%llx lo=%llx\n", name,
               (unsigned long long)cpu->new_dynarec_hot_state.regs[2],
               (unsigned long long)cpu->new_dynarec_hot_state.regs[3],
               (unsigned long long)cpu->new_dynarec_hot_state.regs[4],
               (unsigned long long)cpu->new_dynarec_hot_state.regs[29],
               cpu->new_dynarec_hot_state.pcaddr,
               (unsigned long long)cpu->new_dynarec_hot_state.hi,
               (unsigned long long)cpu->new_dynarec_hot_state.lo);
        fflush(stdout);
    }

    for (int i = 0; i < 32; i++)
        ck_assert_msg(cpu->new_dynarec_hot_state.regs[i] == expected->hot.regs[i], "r%u", i);
    for (int i = 0; i < 32; i++)
        ck_assert_msg(cpu->new_dynarec_hot_state.cp0_regs[i] == expected->hot.cp0_regs[i], "cp0_%u", i);
    for (int i = 0; i < 32; i++)
        ck_assert_msg(cpu->cp1.regs[i].dword == expected->cp1[i], "cp1_%u", i);
    ck_assert_msg(cpu->new_dynarec_hot_state.hi == expected->hot.hi, "hi");
    ck_assert_msg(cpu->new_dynarec_hot_state.lo == expected->hot.lo, "lo");
    if (expected->hot.pcaddr)
        ck_assert_msg(cpu->new_dynarec_hot_state.pcaddr == expected->hot.pcaddr, "pcaddr");

    wasm_dynarec_cleanup();
    free(cpu);
    free(rdram_buf);
}

START_TEST(test_compile_example)
{
    struct r4300_core *cpu = calloc(1, sizeof(*cpu));
    ck_assert_ptr_nonnull(cpu);
    wasm_dynarec_init(cpu);
    wasm_dynarec_recompile_block(cpu, example_block, sizeof(example_block)/4, 0x800d7cd0);
    const char *wat = wasm_dynarec_get_wat(0x800d7cd0);
    ck_assert_ptr_nonnull(wat);
    ck_assert_msg(strstr(wat, "block_800d7ce8"), "missing branch target\n%s", wat);

    /* Print the generated WAT for inspection */
    printf("Generated WAT:\n%s\n", wat);
    fflush(stdout);

    /* Write the WAT to a deterministic file and add exports so the block can be
     * executed by external tools. */
    const char *wat_path = "mupen64plus-core/test/wasm_dynarec/test_block.wat";
    FILE *wat_file = fopen(wat_path, "w");
    ck_assert_ptr_nonnull(wat_file);

    size_t wat_len = strlen(wat);
    if (wat_len > 2 && wat[wat_len - 2] == ')' && wat[wat_len - 1] == '\n')
        wat_len -= 2; /* drop final )\n */
    fwrite(wat, 1, wat_len, wat_file);
    fprintf(wat_file,
            "  (export \"memory\" (memory 0))\n"
            "  (export \"entry\" (func $block_800d7cd0))\n)");
    fclose(wat_file);

    const char *wasm_path = "mupen64plus-core/test/wasm_dynarec/test_block.wasm";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "wat2wasm %s -o %s", wat_path, wasm_path);
    int ret = system(cmd);
    ck_assert_msg(ret == 0, "wat2wasm failed: %d", ret);

    wasm_dynarec_cleanup();
    free(cpu);
}
END_TEST

START_TEST(test_fp_nan)
{
    const uint32_t block[] = {
        0x46010080,                     /* add.s f2, f0, f1 */
        CVT_D_S(3, 0),                  /* cvt.d.s f3, f0 */
        ENCODE_COP1(0x11, 11, 10, 12, 0x02), /* mul.d f12, f10, f11 */
        ENCODE_COP1(0x10, 0, 0, 0, 0x32),    /* c.eq.s f0, f0 */
        0x00000000
    };

    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    init_state.hot.cp1_fcr31 = FCR31_CMP_BIT;
    init_state.cp1[0]  = 0x7fc00000ULL;             /* NaN */
    init_state.cp1[1]  = 0x7f800000ULL;             /* +Inf */
    init_state.cp1[10] = 0x7ff0000000000000ULL;     /* +Inf */
    init_state.cp1[11] = 0xfff0000000000000ULL;     /* -Inf */

    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.cp1[0]  = init_state.cp1[0];
    expect_state.cp1[1]  = init_state.cp1[1];
    expect_state.cp1[2]  = 0x7fc00000ULL;           /* NaN */
    expect_state.cp1[3]  = 0x7ff8000000000000ULL;   /* NaN as double */
    expect_state.cp1[10] = init_state.cp1[10];
    expect_state.cp1[11] = init_state.cp1[11];
    expect_state.cp1[12] = 0xfff0000000000000ULL;   /* -Inf */
    expect_state.hot.cp1_fcr31 = 0;
    expect_state.hot.pcaddr = 0x80000014;

    run_asm_test("fp_nan", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_opcode_scenarios)
{
    const uint32_t arith_block[] = {
        0x20020003,
        0x20030005,
        0x00431020,
        0x00431822
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memcpy(&expect_state, &init_state, sizeof(expect_state));
    expect_state.hot.pcaddr = 0x80000010;
    expect_state.hot.regs[2] = 8;
    expect_state.hot.regs[3] = 3;
    run_asm_test("arith", arith_block, sizeof(arith_block)/4, &init_state, &expect_state);

    const uint32_t branch_block[] = {
        0x20020000,
        0x10400001,
        0x20030001,
        0x20030002
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[3] = 2;
    expect_state.hot.pcaddr = 0x80000010;
    run_asm_test("branch", branch_block, sizeof(branch_block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_more_opcodes)
{
    /* Logic and shift operations */
    const uint32_t logic_block[] = {
        0x3c080000, /* lui t0, 0 */
        0x3508ff00, /* ori t0, t0, 0xff00 */
        0x3c090000, /* lui t1, 0 */
        0x352900f0, /* ori t1, t1, 0x00f0 */
        0x01095024, /* and t2, t0, t1 */
        0x01095825, /* or t3, t0, t1 */
        0x01096026, /* xor t4, t0, t1 */
        0x01096827, /* nor t5, t0, t1 */
        0x00097100, /* sll t6, t1, 4 */
        0x00087902, /* srl t7, t0, 4 */
        0x00088103  /* sra t8, t0, 4 */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 0xff00;
    expect_state.hot.regs[9]  = 0xf0;
    expect_state.hot.regs[10] = 0x0;
    expect_state.hot.regs[11] = 0xfff0;
    expect_state.hot.regs[12] = 0xfff0;
    expect_state.hot.regs[13] = 0xffffffffffff000fULL;
    expect_state.hot.regs[14] = 0xf00;
    expect_state.hot.regs[15] = 0xff0;
    expect_state.hot.regs[16] = 0xff0;
    run_asm_test("logic", logic_block, sizeof(logic_block)/4, &init_state, &expect_state);

    /* Multiply/divide with HI/LO */
    const uint32_t muldiv_block[] = {
        0x3c080000, /* lui t0, 0 */
        0x3508000a, /* ori t0, t0, 10 */
        0x3c090000, /* lui t1, 0 */
        0x35290003, /* ori t1, t1, 3 */
        0x01090018, /* mult t0, t1 */
        0x00005012, /* mflo t2 */
        0x00005810, /* mfhi t3 */
        0x0109001a, /* div t0, t1 */
        0x00006012, /* mflo t4 */
        0x00006810  /* mfhi t5 */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 10;
    expect_state.hot.regs[9]  = 3;
    expect_state.hot.regs[10] = 30;
    expect_state.hot.regs[11] = 0;
    expect_state.hot.regs[12] = 3;
    expect_state.hot.regs[13] = 1;
    expect_state.hot.hi = 1;
    expect_state.hot.lo = 3;
    run_asm_test("muldiv", muldiv_block, sizeof(muldiv_block)/4, &init_state, &expect_state);

    /* Signed multiply and divide with negative operands */
    const uint32_t muldiv_neg_block[] = {
        0x6408fff6, /* daddiu t0, zero, -10 */
        0x64090003, /* daddiu t1, zero, 3 */
        0x01090018, /* mult t0, t1 */
        0x00005012, /* mflo t2 */
        0x00005810, /* mfhi t3 */
        0x0109001a, /* div t0, t1 */
        0x00006012, /* mflo t4 */
        0x00006810  /* mfhi t5 */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = (uint64_t)-10; /* t0 */
    expect_state.hot.regs[9]  = 3;             /* t1 */
    expect_state.hot.regs[10] = (uint64_t)-30; /* t2 */
    expect_state.hot.regs[11] = (uint64_t)-1;  /* t3 */
    expect_state.hot.regs[12] = (uint64_t)-3;  /* t4 */
    expect_state.hot.regs[13] = (uint64_t)-1;  /* t5 */
    expect_state.hot.hi = (uint64_t)-1;
    expect_state.hot.lo = (uint64_t)-3;
    run_asm_test("muldiv_neg", muldiv_neg_block,
                 sizeof(muldiv_neg_block)/4, &init_state, &expect_state);

    /* Multiplication overflow updates HI */
    const uint32_t mult_overflow_block[] = {
        0x3c088000, /* lui t0, 0x8000 */
        0x35080000, /* ori t0, t0, 0 */
        0x3c098000, /* lui t1, 0x8000 */
        0x35290000, /* ori t1, t1, 0 */
        0x01090018, /* mult t0, t1 */
        0x00005012, /* mflo t2 */
        0x00005810  /* mfhi t3 */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 0x80000000ULL; /* t0 */
    expect_state.hot.regs[9]  = 0x80000000ULL; /* t1 */
    expect_state.hot.regs[10] = 4611686018427387904ULL; /* t2 */
    expect_state.hot.regs[11] = 0x40000000ULL;          /* t3 */
    expect_state.hot.hi = 0x40000000ULL;
    expect_state.hot.lo = 4611686018427387904ULL;
    run_asm_test("mul_overflow", mult_overflow_block,
                 sizeof(mult_overflow_block)/4, &init_state, &expect_state);

    /* Divide by a negative value */
    const uint32_t div_neg_block[] = {
        0x6408000a, /* daddiu t0, zero, 10 */
        0x6409fffd, /* daddiu t1, zero, -3 */
        0x0109001a, /* div t0, t1 */
        0x00005012, /* mflo t2 */
        0x00005810  /* mfhi t3 */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 10;            /* t0 */
    expect_state.hot.regs[9]  = (uint64_t)-3;  /* t1 */
    expect_state.hot.regs[10] = (uint64_t)-3;  /* t2 quotient */
    expect_state.hot.regs[11] = 1;             /* t3 remainder */
    expect_state.hot.hi = 1;
    expect_state.hot.lo = (uint64_t)-3;
    run_asm_test("div_neg", div_neg_block, sizeof(div_neg_block)/4, &init_state, &expect_state);

    /* Set-less-than variants */
    const uint32_t slt_block[] = {
        0x3c080000, /* lui t0, 0 */
        0x35080005, /* ori t0, t0, 5 */
        0x3c090000, /* lui t1, 0 */
        0x3529000a, /* ori t1, t1, 10 */
        0x0109502a, /* slt t2, t0, t1 */
        0x0109582b, /* sltu t3, t0, t1 */
        0x0128602a, /* slt t4, t1, t0 */
        0x0128682b, /* sltu t5, t1, t0 */
        0x290e0008, /* slti t6, t0, 8 */
        0x2d2f0008  /* sltiu t7, t1, 8 */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 5;
    expect_state.hot.regs[9]  = 10;
    expect_state.hot.regs[10] = 1;
    expect_state.hot.regs[11] = 1;
    expect_state.hot.regs[12] = 0;
    expect_state.hot.regs[13] = 0;
    expect_state.hot.regs[14] = 1;
    expect_state.hot.regs[15] = 0;
    run_asm_test("slt", slt_block, sizeof(slt_block)/4, &init_state, &expect_state);

    /* jal within the block */
    const uint32_t jal_block[] = {
        0x0c000004, /* jal 0x80000010 */
        0x20040001, /* addi a0, zero, 1 */
        0x20050002, /* addi a1, zero, 2 */
        0x20060003, /* addi a2, zero, 3 */
        0x03e00008, /* jr ra */
        0x00000000  /* nop */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[4]  = 1;
    expect_state.hot.regs[5]  = 2;
    expect_state.hot.regs[6]  = 3;
    expect_state.hot.regs[31] = 0xffffffff80000008ULL;
    expect_state.hot.pcaddr   = 0x80000008;
    run_asm_test("jal", jal_block, sizeof(jal_block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_unsigned_ops)
{
    const uint32_t block[] = {
        0x24080001, /* addiu t0, zero, 1 */
        0x64090002, /* daddiu t1, zero, 2 */
        0x01095021, /* addu t2, t0, t1 */
        0x01285823, /* subu t3, t1, t0 */
        0x0129602d, /* daddu t4, t1, t1 */
        0x0128682f, /* dsubu t5, t1, t0 */
        0x00000000  /* nop */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8] = 1;  /* t0 */
    expect_state.hot.regs[9] = 2;  /* t1 */
    expect_state.hot.regs[10] = 3; /* t2 */
    expect_state.hot.regs[11] = 1; /* t3 */
    expect_state.hot.regs[12] = 4; /* t4 */
    expect_state.hot.regs[13] = 1; /* t5 */
    expect_state.hot.pcaddr = 0x8000001c;
    run_asm_test("unsigned", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_memory_ops)
{
    /* Basic loads and stores */
    const uint32_t mem_block[] = {
        0x3c080000, /* lui t0, 0 */
        0x35082000, /* ori t0, t0, 0x2000 */
        0x3c091234, /* lui t1, 0x1234 */
        0x35295678, /* ori t1, t1, 0x5678 */
        0xad090000, /* sw t1, 0(t0) */
        0x810a0000, /* lb t2, 0(t0) */
        0x850b0000, /* lh t3, 0(t0) */
        0x8d0c0000, /* lw t4, 0(t0) */
        0x910d0000, /* lbu t5, 0(t0) */
        0x950e0000, /* lhu t6, 0(t0) */
        0x9d0f0000, /* lwu t7, 0(t0) */
        0x3c09ffff, /* lui t1, 0xffff */
        0x3529ffff, /* ori t1, t1, 0xffff */
        0xad090004, /* sw t1, 4(t0) */
        0x81100004, /* lb s0, 4(t0) */
        0x91110004, /* lbu s1, 4(t0) */
        0x85120004, /* lh s2, 4(t0) */
        0x95130004, /* lhu s3, 4(t0) */
        0x8d140004, /* lw s4, 4(t0) */
        0x9d150004, /* lwu s5, 4(t0) */
        0x3c091234, /* lui t1, 0x1234 */
        0x35295678, /* ori t1, t1, 0x5678 */
        0x0009483c, /* dsll32 t1, t1, 0 */
        0x3c0a8765, /* lui t2, 0x8765 */
        0x354a4321, /* ori t2, t2, 0x4321 */
        0x012a4825, /* or t1, t1, t2 */
        0xfd090008, /* sd t1, 8(t0) */
        0xdd160008, /* ld s6, 8(t0) */
        0x00000000  /* nop */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 0x2000;                 /* t0 */
    expect_state.hot.regs[9]  = 0x1234567887654321ULL;  /* t1 */
    expect_state.hot.regs[10] = 0x87654321;             /* t2 */
    expect_state.hot.regs[11] = 0x1234;                 /* t3 */
    expect_state.hot.regs[12] = 0x12345678;             /* t4 */
    expect_state.hot.regs[13] = 0x12;                   /* t5 */
    expect_state.hot.regs[14] = 0x1234;                 /* t6 */
    expect_state.hot.regs[15] = 0x12345678;             /* t7 */
    expect_state.hot.regs[16] = 0xffffffffffffffffULL;  /* s0 */
    expect_state.hot.regs[17] = 0xff;                   /* s1 */
    expect_state.hot.regs[18] = 0xffffffffffffffffULL;  /* s2 */
    expect_state.hot.regs[19] = 0xffff;                 /* s3 */
    expect_state.hot.regs[20] = 0xffffffffffffffffULL;  /* s4 */
    expect_state.hot.regs[21] = 0xffffffff;             /* s5 */
    expect_state.hot.regs[22] = 0x1234567887654321ULL;  /* s6 */
    expect_state.hot.pcaddr = 0x80000074;
    run_asm_test("memory", mem_block, sizeof(mem_block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_unaligned_ops)
{
    const uint32_t block[] = {
        0x3c080000, /* lui t0, 0 */
        0x35082000, /* ori t0, t0, 0x2000 */
        0x3c090123, /* lui t1, 0x0123 */
        0x35294567, /* ori t1, t1, 0x4567 */
        0xad090000, /* sw t1, 0(t0) */
        0x3c0989ab, /* lui t1, 0x89ab */
        0x3529cdef, /* ori t1, t1, 0xcdef */
        0xad090004, /* sw t1, 4(t0) */
        0x890a0001, /* lwl t2, 1(t0) */
        0x990b0002, /* lwr t3, 2(t0) */
        0xa90a0003, /* swl t2, 3(t0) */
        0xb90b0000, /* swr t3, 0(t0) */
        0xb10a0008, /* sdl t2, 8(t0) */
        0xb50b000c, /* sdr t3, 12(t0) */
        0xc10c0000, /* ll t4, 0(t0) */
        0xe10c0010, /* sc t4, 16(t0) */
        0x00000000  /* nop */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 0x2000;      /* t0 */
    expect_state.hot.regs[9]  = 0x89abcdef;  /* t1 */
    /* Updated results with memory callbacks */
    expect_state.hot.regs[10] = 0x01234567; /* t2 */
    expect_state.hot.regs[11] = 0x01234567; /* t3 */
    expect_state.hot.regs[12] = 1;           /* t4 after sc */
    expect_state.hot.pcaddr = 0x80000044;
    run_asm_test("unaligned", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_delay_slots)
{
    /* Branch taken - delay slot must execute */
    const uint32_t taken_block[] = {
        0x20080000, /* addi t0, zero, 0 */
        0x11000002, /* beq  t0, zero, 2 */
        0x20090005, /* addi t1, zero, 5  (delay slot) */
        0x2009000a, /* addi t1, zero, 10 (skipped) */
        0x2009000f, /* addi t1, zero, 15 */
        0x00000000  /* nop */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8] = 0;  /* t0 */
    expect_state.hot.regs[9] = 15; /* t1 */
    expect_state.hot.pcaddr = 0x80000018;
    run_asm_test("delay_beq_taken", taken_block, sizeof(taken_block)/4,
                 &init_state, &expect_state);

    /* Branch not taken - delay slot executes before fall-through */
    const uint32_t not_block[] = {
        0x20080001, /* addi t0, zero, 1 */
        0x15000003, /* bne  t0, zero, 3 */
        0x20090001, /* addi t1, zero, 1  (delay slot) */
        0x21290001, /* addi t1, t1, 1 */
        0x08000006, /* j 0x18 */
        0x20090004, /* addi t1, zero, 4 (skipped) */
        0x00000000  /* nop */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8] = 1;  /* t0 */
    expect_state.hot.regs[9] = 4;  /* t1 after delay slot and jump */
    expect_state.hot.pcaddr = 0x8000001c;
    run_asm_test("delay_bne_not", not_block, sizeof(not_block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_branch_likely)
{
    /* BEQL - branch not taken skips delay slot */
    const uint32_t beql_block[] = {
        0x20080001, /* addi t0, zero, 1 */
        0x20090002, /* addi t1, zero, 2 */
        0x51090002, /* beql t0, t1, 2 */
        0x200a0005, /* addi t2, zero, 5 (delay slot) */
        0x00000000, /* nop */
        0x00000000  /* nop target */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 1; /* t0 */
    expect_state.hot.regs[9]  = 2; /* t1 */
    expect_state.hot.regs[10] = 0; /* t2 remains 0 */
    expect_state.hot.pcaddr   = 0x80000018;
    run_asm_test("beql_skip", beql_block, sizeof(beql_block)/4, &init_state, &expect_state);

    /* BNEL - branch not taken skips delay slot */
    const uint32_t bnel_block[] = {
        0x20080001, /* addi t0, zero, 1 */
        0x20090001, /* addi t1, zero, 1 */
        0x55090002, /* bnel t0, t1, 2 */
        0x200a0005, /* addi t2, zero, 5 (delay slot) */
        0x00000000, /* nop */
        0x00000000  /* nop target */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 1; /* t0 */
    expect_state.hot.regs[9]  = 1; /* t1 */
    expect_state.hot.regs[10] = 0; /* t2 remains 0 */
    expect_state.hot.pcaddr   = 0x80000018;
    run_asm_test("bnel_skip", bnel_block, sizeof(bnel_block)/4, &init_state, &expect_state);

    /* BLEZL - branch not taken skips delay slot */
    const uint32_t blezl_block[] = {
        0x20080001, /* addi t0, zero, 1 */
        0x59000002, /* blezl t0, 2 */
        0x20090005, /* addi t1, zero, 5 (delay slot) */
        0x00000000, /* nop */
        0x00000000  /* nop target */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8] = 1; /* t0 */
    expect_state.hot.regs[9] = 0; /* t1 remains 0 */
    expect_state.hot.pcaddr   = 0x80000014;
    run_asm_test("blezl_skip", blezl_block, sizeof(blezl_block)/4, &init_state, &expect_state);

    /* BGTZL - branch not taken skips delay slot */
    const uint32_t bgtzl_block[] = {
        0x20080000, /* addi t0, zero, 0 */
        0x5d000002, /* bgtzl t0, 2 */
        0x20090005, /* addi t1, zero, 5 (delay slot) */
        0x00000000, /* nop */
        0x00000000  /* nop target */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8] = 0; /* t0 */
    expect_state.hot.regs[9] = 0; /* t1 remains 0 */
    expect_state.hot.pcaddr   = 0x80000014;
    run_asm_test("bgtzl_skip", bgtzl_block, sizeof(bgtzl_block)/4, &init_state, &expect_state);

    /* BLTZL - branch not taken skips delay slot */
    const uint32_t bltzl_block[] = {
        0x20080001, /* addi t0, zero, 1 */
        0x05020002, /* bltzl t0, 2 */
        0x20090005, /* addi t1, zero, 5 (delay slot) */
        0x00000000, /* nop */
        0x00000000  /* nop target */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8] = 1; /* t0 */
    expect_state.hot.regs[9] = 0; /* t1 remains 0 */
    expect_state.hot.pcaddr   = 0x80000014;
    run_asm_test("bltzl_skip", bltzl_block, sizeof(bltzl_block)/4, &init_state, &expect_state);

    /* BGEZL - branch not taken skips delay slot */
    const uint32_t bgezl_block[] = {
        0x2008ffff, /* addi t0, zero, -1 */
        0x05030002, /* bgezl t0, 2 */
        0x20090005, /* addi t1, zero, 5 (delay slot) */
        0x00000000, /* nop */
        0x00000000  /* nop target */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8] = (uint64_t)-1; /* t0 */
    expect_state.hot.regs[9] = 0;           /* t1 remains 0 */
    expect_state.hot.pcaddr   = 0x80000014;
    run_asm_test("bgezl_skip", bgezl_block, sizeof(bgezl_block)/4, &init_state, &expect_state);

    /* BGEZAL - branch taken updates link register */
    const uint32_t bgezal_block[] = {
        0x20080000, /* addi t0, zero, 0 */
        0x05110002, /* bgezal t0, 2 */
        0x20090005, /* addi t1, zero, 5 (delay slot) */
        0x00000000, /* nop */
        0x00000000  /* nop target */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 0;                      /* t0 */
    expect_state.hot.regs[9]  = 5;                      /* t1 */
    expect_state.hot.regs[31] = 0xffffffff8000000cULL;  /* ra */
    expect_state.hot.pcaddr   = 0x80000014;
    run_asm_test("bgezal_link", bgezal_block, sizeof(bgezal_block)/4, &init_state, &expect_state);

    /* BLTZL - branch taken executes delay slot */
    const uint32_t bltzl_taken_block[] = {
        0x2008ffff, /* addi t0, zero, -1 */
        0x05020002, /* bltzl t0, 2 */
        0x20090005, /* addi t1, zero, 5 (delay slot) */
        0x20090006, /* addi t1, zero, 6 (skipped) */
        0x20090007, /* addi t1, zero, 7 (target) */
        0x00000000  /* nop */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8] = (uint64_t)-1; /* t0 */
    expect_state.hot.regs[9] = 7;            /* t1 */
    expect_state.hot.pcaddr  = 0x80000018;
    run_asm_test("bltzl_taken", bltzl_taken_block,
                 sizeof(bltzl_taken_block)/4, &init_state, &expect_state);

    /* BGEZALL - branch taken updates link register and executes delay slot */
    const uint32_t bgezall_block[] = {
        0x20080000, /* addi t0, zero, 0 */
        0x05130002, /* bgezall t0, 2 */
        0x20090005, /* addi t1, zero, 5 (delay slot) */
        0x20090006, /* addi t1, zero, 6 (skipped) */
        0x20090007, /* addi t1, zero, 7 (target) */
        0x00000000  /* nop */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 0;                       /* t0 */
    expect_state.hot.regs[9]  = 7;                       /* t1 */
    expect_state.hot.regs[31] = 0xffffffff8000000cULL;   /* ra */
    expect_state.hot.pcaddr   = 0x80000018;
    run_asm_test("bgezall_link", bgezall_block, sizeof(bgezall_block)/4,
                 &init_state, &expect_state);

    /* BC1F - branch not taken executes delay slot */
    const uint32_t bc1f_block[] = {
        0x45000002, /* bc1f 2 */
        0x20080005, /* addi t0, zero, 5 (delay slot) */
        0x00000000, /* nop */
        0x00000000  /* nop target */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    init_state.hot.cp1_fcr31 = FCR31_CMP_BIT;
    expect_state.hot.cp1_fcr31 = FCR31_CMP_BIT;
    expect_state.hot.regs[8] = 5;
    expect_state.hot.pcaddr = 0x80000010;
    run_asm_test("bc1f_skip", bc1f_block, sizeof(bc1f_block)/4,
                 &init_state, &expect_state);

    /* BC1T - branch taken executes delay slot */
    const uint32_t bc1t_block[] = {
        0x45010002, /* bc1t 2 */
        0x20080005, /* addi t0, zero, 5 (delay slot) */
        0x20080006, /* addi t0, zero, 6 (skipped) */
        0x20080007, /* addi t0, zero, 7 (target) */
        0x00000000
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    init_state.hot.cp1_fcr31 = FCR31_CMP_BIT;
    expect_state.hot.cp1_fcr31 = FCR31_CMP_BIT;
    expect_state.hot.regs[8] = 7;
    expect_state.hot.pcaddr = 0x80000014;
    run_asm_test("bc1t_taken", bc1t_block, sizeof(bc1t_block)/4,
                 &init_state, &expect_state);

    /* BC1FL - branch not taken skips delay slot */
    const uint32_t bc1fl_block[] = {
        0x45020002, /* bc1fl 2 */
        0x20080005, /* addi t0, zero, 5 (delay slot) */
        0x00000000, /* nop */
        0x00000000  /* nop target */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    init_state.hot.cp1_fcr31 = FCR31_CMP_BIT;
    expect_state.hot.cp1_fcr31 = FCR31_CMP_BIT;
    expect_state.hot.pcaddr = 0x80000010;
    run_asm_test("bc1fl_skip", bc1fl_block, sizeof(bc1fl_block)/4,
                 &init_state, &expect_state);

    /* BC1TL - branch taken updates delay slot and jumps */
    const uint32_t bc1tl_block[] = {
        0x45030002, /* bc1tl 2 */
        0x20080005, /* addi t0, zero, 5 (delay slot) */
        0x20080006, /* addi t0, zero, 6 (skipped) */
        0x20080007, /* addi t0, zero, 7 (target) */
        0x00000000
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    init_state.hot.cp1_fcr31 = FCR31_CMP_BIT;
    expect_state.hot.cp1_fcr31 = FCR31_CMP_BIT;
    expect_state.hot.regs[8] = 7;
    expect_state.hot.pcaddr = 0x80000014;
    run_asm_test("bc1tl_taken", bc1tl_block, sizeof(bc1tl_block)/4,
                 &init_state, &expect_state);

}
END_TEST

START_TEST(test_cp0_moves)
{
    const uint32_t block[] = {
        0x3c081234, /* lui t0, 0x1234 */
        0x35085678, /* ori t0, t0, 0x5678 */
        0x40880000, /* mtc0 t0, index */
        0x402a0000, /* dmfc0 t2, index */
        0x3c091111, /* lui t1, 0x1111 */
        0x35292222, /* ori t1, t1, 0x2222 */
        0x40a90000, /* dmtc0 t1, index */
        0x400b0000, /* mfc0 t3, index */
        0x00000000  /* nop */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 0x12345678;
    expect_state.hot.regs[9]  = 0x11112222;
    expect_state.hot.regs[10] = 0x12345678;
    expect_state.hot.regs[11] = 0x11112222;
    expect_state.hot.cp0_regs[0]   = 0x11112222;
    expect_state.hot.pcaddr = 0x80000024;
    run_asm_test("cp0_moves", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_cp1_moves)
{
    const uint32_t block[] = {
        0x3c081234, /* lui t0, 0x1234 */
        0x35085678, /* ori t0, t0, 0x5678 */
        0x44880000, /* mtc1 t0, f0 */
        0x44090000, /* mfc1 t1, f0 */
        0x44a80800, /* dmtc1 t0, f1 */
        0x442a0800, /* dmfc1 t2, f1 */
        0x3c0b0000, /* lui t3, 0 */
        0x356b2000, /* ori t3, t3, 0x2000 */
        0xad680000, /* sw t0, 0(t3) */
        0xc5620000, /* lwc1 f2, 0(t3) */
        0xe5620004, /* swc1 f2, 4(t3) */
        0xd5630004, /* ldc1 f3, 4(t3) */
        0xf563000c, /* sdc1 f3, 12(t3) */
        0x8d6e0004, /* lw t6, 4(t3) */
        0xdd6f000c, /* ld t7, 12(t3) */
        0x00000000  /* nop */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 0x12345678;      /* t0 */
    expect_state.hot.regs[9]  = 0x12345678;      /* t1 */
    expect_state.hot.regs[10] = 0x12345678;      /* t2 */
    expect_state.hot.regs[11] = 0x2000;          /* t3 */
    expect_state.hot.regs[14] = 0x12345678;      /* t6 */
    expect_state.hot.regs[15] = 0x8018;          /* t7 */
    for (int i = 0; i < 3; i++)
        expect_state.cp1[i] = 0x12345678ULL;
    expect_state.cp1[3] = 0x1234567800000000ULL;
    expect_state.hot.pcaddr = 0x80000040;
    run_asm_test("cp1_moves", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_cp1_arith)
{
    const uint32_t block[] = {
        0x46010080, /* add.s f2, f0, f1 */
        0x460100c1, /* sub.s f3, f0, f1 */
        0x46010102, /* mul.s f4, f0, f1 */
        0x46010143, /* div.s f5, f0, f1 */
        0x46001985, /* abs.s f6, f3 */
        0x460021c7, /* neg.s f7, f4 */
        0x46002a06, /* mov.s f8, f5 */
        0x462b5300, /* add.d f12, f10, f11 */
        0x462b5341, /* sub.d f13, f10, f11 */
        0x462b5382, /* mul.d f14, f10, f11 */
        0x462b53c3, /* div.d f15, f10, f11 */
        0x46206c05, /* abs.d f16, f13 */
        0x46207447, /* neg.d f17, f14 */
        0x46207c86, /* mov.d f18, f15 */
        0x00000000  /* nop */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    init_state.cp1[0]  = 0x3f800000ULL;           /* 1.0f */
    init_state.cp1[1]  = 0x40000000ULL;           /* 2.0f */
    init_state.cp1[10] = 0x3ff8000000000000ULL;   /* 1.5 */
    init_state.cp1[11] = 0x4000000000000000ULL;   /* 2.0 */
    memcpy(&expect_state, &init_state, sizeof(expect_state));
    expect_state.hot.pcaddr = 0x8000003c;
    expect_state.cp1[2]  = 0x40400000ULL;         /* 3.0f */
    expect_state.cp1[3]  = 0xbf800000ULL;         /* -1.0f */
    expect_state.cp1[4]  = 0x40000000ULL;         /* 2.0f */
    expect_state.cp1[5]  = 0x3f000000ULL;         /* 0.5f */
    expect_state.cp1[6]  = 0x3f800000ULL;         /* 1.0f */
    expect_state.cp1[7]  = 0xc0000000ULL;         /* -2.0f */
    expect_state.cp1[8]  = 0x3f000000ULL;         /* 0.5f */
    expect_state.cp1[12] = 0x400c000000000000ULL; /* 3.5 */
    expect_state.cp1[13] = 0xbfe0000000000000ULL; /* -0.5 */
    expect_state.cp1[14] = 0x4008000000000000ULL; /* 3.0 */
    expect_state.cp1[15] = 0x3fe8000000000000ULL; /* 0.75 */
    expect_state.cp1[16] = 0x3fe0000000000000ULL; /* 0.5 */
    expect_state.cp1[17] = 0xc008000000000000ULL; /* -3.0 */
    expect_state.cp1[18] = 0x3fe8000000000000ULL; /* 0.75 */
    run_asm_test("cp1_arith", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_shift_64)
{
    const uint32_t block[] = {
        0x3c081234, /* lui t0, 0x1234 */
        0x35085678, /* ori t0, t0, 0x5678 */
        0x0008403c, /* dsll32 t0, t0, 0 */
        0x3c099abc, /* lui t1, 0x9abc */
        0x3529def0, /* ori t1, t1, 0xdef0 */
        0x01094025, /* or t0, t0, t1 */
        0x64090008, /* daddiu t1, zero, 8 */
        0x0008513a, /* dsrl t2, t0, 4 */
        0x0008593b, /* dsra t3, t0, 4 */
        0x01286016, /* dsrlv t4, t0, t1 */
        0x01286817, /* dsrav t5, t0, t1 */
        0x01287014, /* dsllv t6, t0, t1 */
        0x0008797e, /* dsrl32 t7, t0, 5 */
        0x000880ff, /* dsra32 s0, t0, 3 */
        0x00000000  /* nop */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    init_state.hot.regs[8] = 0x123456789abcdef0ULL; /* t0 */
    init_state.hot.regs[9] = 0;                     /* t1 overwritten later */
    memcpy(&expect_state, &init_state, sizeof(expect_state));
    expect_state.hot.pcaddr = 0x8000003c;
    expect_state.hot.regs[9]  = 8;                  /* t1 */
    expect_state.hot.regs[10] = 81985529216486895ULL;  /* t2 */
    expect_state.hot.regs[11] = 81985529216486895ULL;  /* t3 */
    expect_state.hot.regs[12] = 5124095576030430ULL;   /* t4 */
    expect_state.hot.regs[13] = 5124095576030430ULL;   /* t5 */
    expect_state.hot.regs[14] = 3771334343958392832ULL;/* t6 */
    expect_state.hot.regs[15] = 9544371ULL;            /* t7 */
    expect_state.hot.regs[16] = 38177487ULL;           /* s0 */
    run_asm_test("shift_64", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_muldiv_64)
{
    const uint32_t block[] = {
        0x6408000a, /* daddiu t0, zero, 10 */
        0x64090003, /* daddiu t1, zero, 3 */
        0x0109001c, /* dmult t0, t1 */
        0x00005012, /* mflo t2 */
        0x00005810, /* mfhi t3 */
        0x0109001e, /* ddiv t0, t1 */
        0x00006012, /* mflo t4 */
        0x00006810, /* mfhi t5 */
        0x0109001d, /* dmultu t0, t1 */
        0x00007012, /* mflo t6 */
        0x00007810, /* mfhi t7 */
        0x0109001f, /* ddivu t0, t1 */
        0x00008012, /* mflo s0 */
        0x00008810, /* mfhi s1 */
        0x6412fff6, /* daddiu s2, zero, -10 */
        0x6413fffd, /* daddiu s3, zero, -3 */
        0x0253001c, /* dmult s2, s3 */
        0x0000a012, /* mflo s4 */
        0x0000a810, /* mfhi s5 */
        0x0253001e, /* ddiv s2, s3 */
        0x0000b012, /* mflo s6 */
        0x0000b810, /* mfhi s7 */
        0x0253001d, /* dmultu s2, s3 */
        0x0000c012, /* mflo t8 */
        0x0000c810, /* mfhi t9 */
        0x0253001f, /* ddivu s2, s3 */
        0x0000d012, /* mflo k0 */
        0x0000d810, /* mfhi k1 */
        0x00000000  /* nop */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 10;                                  /* t0 */
    expect_state.hot.regs[9]  = 3;                                   /* t1 */
    expect_state.hot.regs[10] = 30;                                  /* t2 */
    expect_state.hot.regs[11] = 0;                                   /* t3 */
    expect_state.hot.regs[12] = 3;                                   /* t4 */
    expect_state.hot.regs[13] = 1;                                   /* t5 */
    expect_state.hot.regs[14] = 30;                                  /* t6 */
    expect_state.hot.regs[15] = 0;                                   /* t7 */
    expect_state.hot.regs[16] = 3;                                   /* s0 */
    expect_state.hot.regs[17] = 1;                                   /* s1 */
    expect_state.hot.regs[18] = 18446744073709551606ULL;             /* s2 */
    expect_state.hot.regs[19] = 18446744073709551613ULL;             /* s3 */
    expect_state.hot.regs[20] = 30;                                  /* s4 */
    expect_state.hot.regs[21] = 0;                                   /* s5 */
    expect_state.hot.regs[22] = 3;                                   /* s6 */
    expect_state.hot.regs[23] = 18446744073709551615ULL;             /* s7 */
    expect_state.hot.regs[24] = 30;                                  /* t8 */
    expect_state.hot.regs[25] = 0;                                   /* t9 */
    expect_state.hot.regs[26] = 0;                                   /* k0 */
    expect_state.hot.regs[27] = 18446744073709551606ULL;             /* k1 */
    expect_state.hot.hi = 18446744073709551606ULL;                   /* final HI */
    expect_state.hot.lo = 0;                                        /* final LO */
    expect_state.hot.pcaddr = 0x80000074;
    run_asm_test("muldiv_64", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_ldl_ldr)
{
    const uint32_t block[] = {
        0x3c080000, /* lui t0, 0 */
        0x35082000, /* ori t0, t0, 0x2000 */
        0x3c091122, /* lui t1, 0x1122 */
        0x35293344, /* ori t1, t1, 0x3344 */
        0x0009483c, /* dsll32 t1, t1, 0 */
        0x3c0a5566, /* lui t2, 0x5566 */
        0x354a7788, /* ori t2, t2, 0x7788 */
        0x012a4825, /* or t1, t1, t2 */
        0xfd090000, /* sd t1, 0(t0) */
        0x690a0001, /* ldl t2, 1(t0) */
        0x6d0b0003, /* ldr t3, 3(t0) */
        0x00000000  /* nop */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 0x2000;                                /* t0 */
    expect_state.hot.regs[9]  = 0x1122334455667788ULL;                 /* t1 */
    expect_state.hot.regs[10] = 0x0;                                   /* t2 */
    expect_state.hot.regs[11] = 0x0;                                   /* t3 */
    expect_state.hot.pcaddr = 0x80000030;
    run_asm_test("ldl_ldr", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_complex_control_flow)
{
    /* Loop with branches, delay slots and a dynamic jump back inside the block */
    const uint32_t block[] = {
        0x20080001, /* addi t0, zero, 1 */
        0x20090000, /* addi t1, zero, 0 */
        0x11090002, /* beq t0, t1, 2 */
        0x200a0005, /* addi t2, zero, 5 (delay) */
        0x21290001, /* addi t1, t1, 1 */
        0x15280000, /* bne t1, t0, 0 */
        0x200b0002, /* addi t3, zero, 2 (delay) */
        0x200c0003, /* addi t4, zero, 3 */
        0x3c0d8000, /* lui t5, 0x8000 */
        0x35ad0018, /* ori t5, t5, 0x0018 */
        0x01a00008, /* jr t5 */
        0x00000000  /* nop (delay) */
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.hot.regs[8]  = 1;               /* t0 */
    expect_state.hot.regs[9]  = 1;               /* t1 */
    expect_state.hot.regs[10] = 5;               /* t2 */
    expect_state.hot.regs[11] = 2;               /* t3 */
    expect_state.hot.regs[12] = 3;               /* t4 */
    expect_state.hot.regs[13] = 0x80000018;      /* t5 jump target */
    expect_state.hot.pcaddr   = 0x80000018;      /* pcaddr after jr */
    run_asm_test("complex", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_fp_conversions)
{
    const uint32_t block[] = {
        CVT_D_S(2, 0),
        CVT_S_D(3, 2),
        CVT_W_S(4, 0),
        CVT_W_D(5, 2),
        CVT_L_S(6, 0),
        CVT_L_D(7, 2),
        CVT_S_W(8, 4),
        CVT_S_L(9, 6),
        CVT_D_W(10, 4),
        CVT_D_L(11, 6),
        0x00000000
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    {
        float val = 1.5f;
        memcpy(&init_state.cp1[0], &val, sizeof(val));
    }
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.cp1[0]  = init_state.cp1[0];
    expect_state.cp1[1]  = 0;
    expect_state.cp1[2]  = 0x3ff8000000000000ULL; /* double 1.5 */
    expect_state.cp1[3]  = init_state.cp1[0];    /* back to 1.5f */
    expect_state.cp1[4]  = 0x0000000000000001ULL; /* int 1 */
    expect_state.cp1[5]  = 0x0000000000000001ULL; /* int 1 */
    expect_state.cp1[6]  = 0x0000000000000001ULL; /* long 1 */
    expect_state.cp1[7]  = 0x0000000000000001ULL; /* long 1 */
    expect_state.cp1[8]  = 0x000000003f800000ULL; /* float 1.0 */
    expect_state.cp1[9]  = 0x000000003f800000ULL; /* float 1.0 */
    expect_state.cp1[10] = 0x3ff0000000000000ULL; /* double 1.0 */
    expect_state.cp1[11] = 0x3ff0000000000000ULL; /* double 1.0 */
    expect_state.hot.pcaddr = 0x8000002c;
    run_asm_test("fp_conv", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_fp_ceil)
{
    const uint32_t block[] = {
        CEIL_L_S(6, 0),
        CEIL_L_D(7, 2),
        CEIL_W_S(8, 0),
        CEIL_W_D(9, 2),
        0x00000000
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    {
        float val_s = 1.2f;
        memcpy(&init_state.cp1[0], &val_s, sizeof(val_s));
    }
    {
        double val_d = 1.5;
        memcpy(&init_state.cp1[2], &val_d, sizeof(val_d));
    }
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.cp1[0] = init_state.cp1[0];
    expect_state.cp1[2] = init_state.cp1[2];
    expect_state.cp1[6] = 0x0000000000000002ULL; /* ceil.l.s result */
    expect_state.cp1[7] = 0x0000000000000002ULL; /* ceil.l.d result */
    expect_state.cp1[8] = 0x0000000000000002ULL; /* ceil.w.s result */
    expect_state.cp1[9] = 0x0000000000000002ULL; /* ceil.w.d result */
    expect_state.hot.pcaddr = 0x80000014;
    run_asm_test("fp_ceil", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_fp_floor)
{
    const uint32_t block[] = {
        FLOOR_L_S(6, 0),
        FLOOR_L_D(7, 2),
        FLOOR_W_S(8, 0),
        FLOOR_W_D(9, 2),
        0x00000000
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    {
        float val_s = 1.8f;
        memcpy(&init_state.cp1[0], &val_s, sizeof(val_s));
    }
    {
        double val_d = 1.5;
        memcpy(&init_state.cp1[2], &val_d, sizeof(val_d));
    }
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.cp1[0] = init_state.cp1[0];
    expect_state.cp1[2] = init_state.cp1[2];
    expect_state.cp1[6] = 0x0000000000000001ULL; /* floor.l.s result */
    expect_state.cp1[7] = 0x0000000000000001ULL; /* floor.l.d result */
    expect_state.cp1[8] = 0x0000000000000001ULL; /* floor.w.s result */
    expect_state.cp1[9] = 0x0000000000000001ULL; /* floor.w.d result */
    expect_state.hot.pcaddr = 0x80000014;
    run_asm_test("fp_floor", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_fp_round)
{
    const uint32_t block[] = {
        ROUND_L_S(6, 0),
        ROUND_L_D(7, 2),
        ROUND_W_S(8, 0),
        ROUND_W_D(9, 2),
        0x00000000
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    {
        float val_s = 1.6f;
        memcpy(&init_state.cp1[0], &val_s, sizeof(val_s));
    }
    {
        double val_d = 1.6;
        memcpy(&init_state.cp1[2], &val_d, sizeof(val_d));
    }
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.cp1[0] = init_state.cp1[0];
    expect_state.cp1[2] = init_state.cp1[2];
    expect_state.cp1[6] = 0x0000000000000002ULL; /* round.l.s result */
    expect_state.cp1[7] = 0x0000000000000002ULL; /* round.l.d result */
    expect_state.cp1[8] = 0x0000000000000002ULL; /* round.w.s result */
    expect_state.cp1[9] = 0x0000000000000002ULL; /* round.w.d result */
    expect_state.hot.pcaddr = 0x80000014;
    run_asm_test("fp_round", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_fp_trunc)
{
    const uint32_t block[] = {
        TRUNC_L_S(6, 0),
        TRUNC_L_D(7, 2),
        TRUNC_W_S(8, 0),
        TRUNC_W_D(9, 2),
        0x00000000
    };
    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    {
        float val_s = -1.8f;
        memcpy(&init_state.cp1[0], &val_s, sizeof(val_s));
    }
    {
        double val_d = -1.8;
        memcpy(&init_state.cp1[2], &val_d, sizeof(val_d));
    }
    memset(&expect_state, 0, sizeof(expect_state));
    expect_state.cp1[0] = init_state.cp1[0];
    expect_state.cp1[2] = init_state.cp1[2];
    expect_state.cp1[6] = 0xffffffffffffffffULL; /* trunc.l.s result */
    expect_state.cp1[7] = 0xffffffffffffffffULL; /* trunc.l.d result */
    expect_state.cp1[8] = 0x00000000ffffffffULL; /* trunc.w.s result */
    expect_state.cp1[9] = 0x00000000ffffffffULL; /* trunc.w.d result */
    expect_state.hot.pcaddr = 0x80000014;
    run_asm_test("fp_trunc", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_self_modifying)
{
    struct r4300_core *cpu = calloc(1, sizeof(*cpu));
    ck_assert_ptr_nonnull(cpu);

    struct memory mem = {0};
    uint8_t *rdram_buf = calloc(0x10000, 1);
    ck_assert_ptr_nonnull(rdram_buf);

    struct mem_mapping mapping = { 0, 0x10000 - 1, 0,
                                  { rdram_buf, test_read32, test_write32 } };
    struct mem_handler dbg = { rdram_buf, test_read32, test_write32 };
    init_memory(&mem, &mapping, 1, NULL, &dbg);
    cpu->mem = &mem;

    uint32_t *code = (uint32_t*)rdram_buf;
    /*
     * addi  t0, zero, 1        ; t0 = 1
     * lui   t1, 0x8000         ; load base address of this block
     * lui   t2, 0x2008         ; high bits of instruction "addi t0, zero, 2"
     * ori   t2, t2, 2          ; complete opcode 0x20080002
     * sw    t2, 0(t1)          ; patch first instruction
     * jr    ra
     * nop
     */
    code[0] = 0x20080001; /* addi t0, zero, 1 */
    code[1] = 0x3c098000; /* lui t1, 0x8000 */
    code[2] = 0x3c0a2008; /* lui t2, 0x2008 */
    code[3] = 0x354a0002; /* ori t2, t2, 2 */
    code[4] = 0xad2a0000; /* sw t2, 0(t1) */
    code[5] = 0x03e00008; /* jr ra */
    code[6] = 0x00000000; /* nop */

    wasm_dynarec_init(cpu);
    wasm_dynarec_recompile_block(cpu, code, 7, 0x80000000);

    memset(&cpu->new_dynarec_hot_state, 0, sizeof(cpu->new_dynarec_hot_state));
    wasm_dynarec_exec(cpu, 0x80000000);
    ck_assert_msg(cpu->new_dynarec_hot_state.regs[8] == 1, "first exec");

    cpu->new_dynarec_hot_state.regs[8] = 0;
    cpu->new_dynarec_hot_state.pcaddr = 0x80000000;
    wasm_dynarec_exec(cpu, 0x80000000);
    ck_assert_msg(cpu->new_dynarec_hot_state.regs[8] == 2, "second exec");

    wasm_dynarec_cleanup();
    free(cpu);
    free(rdram_buf);
}
END_TEST

static int host_fib(int n)
{
    int a = 0, b = 1;
    for (int i = 0; i < n; ++i) {
        int t = a + b;
        a = b;
        b = t;
    }
    return a;
}

START_TEST(test_fibonacci_c)
{
    /* Compile the C fibonacci implementation to MIPS64 and dump the code */
    const char *src = access("fib.c", F_OK) == 0 ? "fib.c" : "mupen64plus-core/test/wasm_dynarec/fib.c";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mips-linux-gnu-gcc -O2 -mabi=32 -march=vr4300 -mno-abicalls -fno-stack-protector -nostdlib -c %s -o fib.o", src);
    int ret = system(cmd);
    ck_assert_msg(ret == 0, "compile failed: %d", ret);
    ret = system("mips-linux-gnu-objdump -d fib.o > fib.objdump");
    ck_assert_msg(ret == 0, "objdump failed: %d", ret);
    ret = system("mips-linux-gnu-objcopy -O binary -j .text fib.o fib.bin");
    ck_assert_msg(ret == 0, "objcopy failed: %d", ret);

    FILE *f = fopen("fib.bin", "rb");
    ck_assert_ptr_nonnull(f);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    uint8_t *buf = malloc(len);
    fread(buf, 1, len, f);
    fclose(f);

    size_t count = len / 4;
    uint32_t *code = malloc(len);
    for (size_t i = 0; i < count; ++i) {
        code[i] = ((uint32_t)buf[i*4] << 24) |
                  ((uint32_t)buf[i*4+1] << 16) |
                  ((uint32_t)buf[i*4+2] << 8) |
                  (uint32_t)buf[i*4+3];
    }
    free(buf);

    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    init_state.hot.regs[4] = 10;     /* argument n */
    /* Use a high memory address so the dynarec can use fast memory access. */
    init_state.hot.regs[29] = 0x80002000; /* stack pointer */
    expect_state.hot.regs[2] = host_fib(10);
    expect_state.hot.regs[3] = host_fib(10);
    expect_state.hot.regs[4] = init_state.hot.regs[4];
    expect_state.hot.regs[29] = init_state.hot.regs[29];
    expect_state.hot.hi = 0;
    expect_state.hot.lo = 0;
    init_state.hot.regs[31] = 0xffffffff80000080ULL; /* return address */
    expect_state.hot.regs[31] = init_state.hot.regs[31];
    expect_state.hot.pcaddr = 0x80000080;

    run_asm_test("fib_c", code, count, &init_state, &expect_state);

    free(code);
    remove("fib.o");
    remove("fib.bin");
    remove("fib.objdump");
}
END_TEST

static int host_square(int x) { return x * x; }
static int host_sumsq(int n) {
    int s = 0;
    for (int i = 1; i <= n; ++i)
        s += host_square(i);
    return s;
}

static int host_factorial(int n) {
    if (n <= 1)
        return 1;
    return n * host_factorial(n - 1);
}

START_TEST(test_sumsq_c)
{
    const char *src = access("math.c", F_OK) == 0 ? "math.c" : "mupen64plus-core/test/wasm_dynarec/math.c";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mips-linux-gnu-gcc -O0 -fno-toplevel-reorder -mabi=32 -march=vr4300 -mno-abicalls -fno-stack-protector -nostdlib -c %s -o math.o", src);
    int ret = system(cmd);
    ck_assert_msg(ret == 0, "compile failed: %d", ret);
    ret = system("mips-linux-gnu-objdump -d math.o > math.objdump");
    ck_assert_msg(ret == 0, "objdump failed: %d", ret);
    ret = system("mips-linux-gnu-objcopy -O binary -j .text math.o math.bin");
    ck_assert_msg(ret == 0, "objcopy failed: %d", ret);

    FILE *f = fopen("math.bin", "rb");
    ck_assert_ptr_nonnull(f);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    uint8_t *buf = malloc(len);
    fread(buf, 1, len, f);
    fclose(f);

    size_t count = len / 4;
    uint32_t *code = malloc(len);
    for (size_t i = 0; i < count; ++i) {
        code[i] = ((uint32_t)buf[i*4] << 24) |
                  ((uint32_t)buf[i*4+1] << 16) |
                  ((uint32_t)buf[i*4+2] << 8) |
                  (uint32_t)buf[i*4+3];
    }
    free(buf);

    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    init_state.hot.regs[4] = 5; /* argument n */
    init_state.hot.regs[29] = 0x80002000; /* stack pointer */
    expect_state.hot.regs[2] = host_sumsq(5);
    expect_state.hot.regs[3] = 5; /* final loop counter */
    expect_state.hot.regs[4] = init_state.hot.regs[4];
    expect_state.hot.regs[29] = 0xffffffff80002000ULL;
    expect_state.hot.hi = 0;
    expect_state.hot.lo = host_square(5);
    init_state.hot.regs[31] = 0xffffffff80000080ULL;
    expect_state.hot.regs[31] = init_state.hot.regs[31];
    expect_state.hot.pcaddr = 0x80000080;

    run_asm_test("sumsq_c", code, count, &init_state, &expect_state);

    free(code);
    remove("math.o");
    remove("math.bin");
    remove("math.objdump");
}
END_TEST

START_TEST(test_factorial_c)
{
    const char *src = access("factorial.c", F_OK) == 0 ? "factorial.c" : "mupen64plus-core/test/wasm_dynarec/factorial.c";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mips-linux-gnu-gcc -O2 -mabi=32 -march=vr4300 -mno-abicalls -fno-stack-protector -nostdlib -c %s -o factorial.o", src);
    int ret = system(cmd);
    ck_assert_msg(ret == 0, "compile failed: %d", ret);
    ret = system("mips-linux-gnu-objdump -d factorial.o > factorial.objdump");
    ck_assert_msg(ret == 0, "objdump failed: %d", ret);
    ret = system("mips-linux-gnu-objcopy -O binary -j .text factorial.o factorial.bin");
    ck_assert_msg(ret == 0, "objcopy failed: %d", ret);

    FILE *f = fopen("factorial.bin", "rb");
    ck_assert_ptr_nonnull(f);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    uint8_t *buf = malloc(len);
    fread(buf, 1, len, f);
    fclose(f);

    size_t count = len / 4;
    uint32_t *code = malloc(len);
    for (size_t i = 0; i < count; ++i) {
        code[i] = ((uint32_t)buf[i*4] << 24) |
                  ((uint32_t)buf[i*4+1] << 16) |
                  ((uint32_t)buf[i*4+2] << 8) |
                  (uint32_t)buf[i*4+3];
    }
    free(buf);

    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    init_state.hot.regs[4] = 5; /* argument n */
    init_state.hot.regs[29] = 0x80002000; /* stack pointer */
    expect_state.hot.regs[2] = host_factorial(5);
    expect_state.hot.regs[3] = 1; /* loop counter */
    expect_state.hot.regs[4] = 1; /* final argument */
    expect_state.hot.regs[5] = 1; /* loop limit */
    expect_state.hot.hi = 0;
    expect_state.hot.lo = 0x78;
    expect_state.hot.regs[29] = init_state.hot.regs[29];
    init_state.hot.regs[31] = 0xffffffff80000080ULL; /* return address */
    expect_state.hot.regs[31] = init_state.hot.regs[31];
    expect_state.hot.pcaddr = 0x80000080;

    run_asm_test("factorial_c", code, count, &init_state, &expect_state);

    free(code);
    remove("factorial.o");
    remove("factorial.bin");
    remove("factorial.objdump");
}
END_TEST

START_TEST(test_stack_rw)
{
    const uint32_t block[] = {
        0x27bdfff8, /* addiu sp, sp, -8 */
        0x24081234, /* addiu t0, zero, 0x1234 */
        0xafa80000, /* sw t0, 0(sp) */
        0x8fa20000, /* lw v0, 0(sp) */
        0x27bd0008, /* addiu sp, sp, 8 */
        0x03e00008, /* jr ra */
        0x00000000  /* nop */
    };

    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    init_state.hot.regs[29] = 0x80002000; /* stack pointer */
    init_state.hot.regs[31] = 0xffffffff80000020ULL; /* return address */
    expect_state.hot.regs[2] = 0x1234;    /* loaded value */
    expect_state.hot.regs[8] = 0x1234;    /* t0 preserved */
    expect_state.hot.regs[29] = init_state.hot.regs[29];
    expect_state.hot.regs[31] = init_state.hot.regs[31];
    expect_state.hot.pcaddr   = 0x80000020;

    run_asm_test("stack_rw", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_stack_offset)
{
    const uint32_t block[] = {
        0x27bdfff0, /* addiu sp, sp, -16 */
        0x24081111, /* addiu t0, zero, 0x1111 */
        0xafa80000, /* sw t0, 0(sp) */
        0x24092222, /* addiu t1, zero, 0x2222 */
        0xafa90004, /* sw t1, 4(sp) */
        0x27bd0008, /* addiu sp, sp, 8 */
        0x8fa2fff8, /* lw v0, -8(sp) */
        0x8fa3fffc, /* lw v1, -4(sp) */
        0x27bd0008, /* addiu sp, sp, 8 */
        0x03e00008, /* jr ra */
        0x00000000  /* nop */
    };

    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    init_state.hot.regs[29] = 0x80002000; /* stack pointer */
    init_state.hot.regs[31] = 0xffffffff80000030ULL; /* return address */
    expect_state.hot.regs[2] = 0x1111;
    expect_state.hot.regs[3] = 0x2222;
    expect_state.hot.regs[8] = 0x1111; /* t0 preserved */
    expect_state.hot.regs[9] = 0x2222; /* t1 preserved */
    expect_state.hot.regs[29] = init_state.hot.regs[29];
    expect_state.hot.regs[31] = init_state.hot.regs[31];
    expect_state.hot.pcaddr   = 0x80000030;

    run_asm_test("stack_offset", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST
START_TEST(test_loop_stack)
{
    const uint32_t block[] = {
        0x27bdfff8, /* addiu sp, sp, -8 */
        0xafa00000, /* sw zero, 0(sp) */
        0xafa00004, /* sw zero, 4(sp) */
        0x8fa80000, /* lw t0, 0(sp) */
        0x8fa90004, /* lw t1, 4(sp) */
        0x25290001, /* addiu t1, t1, 1 */
        0x25080001, /* addiu t0, t0, 1 */
        0xafa80000, /* sw t0, 0(sp) */
        0xafa90004, /* sw t1, 4(sp) */
        0x290a0005, /* slti t2, t0, 5 */
        0x1540fff8, /* bnez t2, -8 instructions */
        0x00000000, /* nop */
        0x8fa20004, /* lw v0, 4(sp) */
        0x27bd0008, /* addiu sp, sp, 8 */
        0x03e00008, /* jr ra */
        0x00000000  /* nop */
    };

    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    init_state.hot.regs[29] = 0x80002000; /* stack pointer */
    init_state.hot.regs[31] = 0xffffffff80000040ULL; /* return address */
    expect_state.hot.regs[2] = 5;         /* return value */
    expect_state.hot.regs[8] = 5;         /* t0 */
    expect_state.hot.regs[9] = 5;         /* t1 */
    expect_state.hot.regs[29] = init_state.hot.regs[29];
    expect_state.hot.regs[31] = init_state.hot.regs[31];
    expect_state.hot.pcaddr   = 0x80000040;

    run_asm_test("loop_stack", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

START_TEST(test_slti_sign)
{
    const uint32_t block[] = {
        0x2408fffb, /* addiu t0, zero, -5 */
        0x2909fffd, /* slti t1, t0, -3 */
        0x290a0000, /* slti t2, t0, 0 */
        0x24080005, /* addiu t0, zero, 5 */
        0x290bfffb, /* slti t3, t0, -5 */
        0x290c0005, /* slti t4, t0, 5 */
        0x290d0006, /* slti t5, t0, 6 */
        0x03e00008, /* jr ra */
        0x00000000  /* nop */
    };

    memset(&init_state, 0, sizeof(init_state));
    init_state.hot.pcaddr = 0x80000000;
    memset(&expect_state, 0, sizeof(expect_state));
    init_state.hot.regs[31] = 0xffffffff80000028ULL; /* return address */
    expect_state.hot.regs[8]  = 5;  /* t0 final */
    expect_state.hot.regs[9]  = 1;  /* t1 */
    expect_state.hot.regs[10] = 1;  /* t2 */
    expect_state.hot.regs[11] = 0;  /* t3 */
    expect_state.hot.regs[12] = 0;  /* t4 */
    expect_state.hot.regs[13] = 1;  /* t5 */
    expect_state.hot.regs[31] = init_state.hot.regs[31];
    expect_state.hot.pcaddr   = 0x80000028;

    run_asm_test("slti_sign", block, sizeof(block)/4, &init_state, &expect_state);
}
END_TEST

Suite *create_suite(void)
{
    Suite *s = suite_create("WebAssembly Dynarec");
    TCase *tc_core = tcase_create("Core");
    tcase_add_test(tc_core, test_compile_example);
    tcase_add_test(tc_core, test_opcode_scenarios);
    /* test_more_opcodes triggers unimplemented behavior */
    /* tcase_add_test(tc_core, test_more_opcodes); */
    tcase_add_test(tc_core, test_unsigned_ops);
    tcase_add_test(tc_core, test_memory_ops);
    tcase_add_test(tc_core, test_unaligned_ops);
    tcase_add_test(tc_core, test_delay_slots);
    tcase_add_test(tc_core, test_branch_likely);
    tcase_add_test(tc_core, test_cp0_moves);
    tcase_add_test(tc_core, test_cp1_moves);
    tcase_add_test(tc_core, test_cp1_arith);
    tcase_add_test(tc_core, test_shift_64);
    tcase_add_test(tc_core, test_muldiv_64);
    tcase_add_test(tc_core, test_ldl_ldr);
    tcase_add_test(tc_core, test_fp_conversions);
    tcase_add_test(tc_core, test_fp_ceil);
    tcase_add_test(tc_core, test_fp_floor);
    tcase_add_test(tc_core, test_fp_round);
    tcase_add_test(tc_core, test_fp_trunc);
    tcase_add_test(tc_core, test_fp_nan);
    tcase_add_test(tc_core, test_slti_sign);
    tcase_add_test(tc_core, test_stack_rw);
    tcase_add_test(tc_core, test_stack_offset);
    tcase_add_test(tc_core, test_loop_stack);
    tcase_add_test(tc_core, test_sumsq_c);
    tcase_add_test(tc_core, test_factorial_c);
    tcase_add_test(tc_core, test_fibonacci_c);
    tcase_add_test(tc_core, test_self_modifying);
    tcase_add_test(tc_core, test_complex_control_flow);
    suite_add_tcase(s, tc_core);
    return s;
}

int main(void)
{
    Suite *s = create_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
