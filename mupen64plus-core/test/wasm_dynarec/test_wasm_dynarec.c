#include <check.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "device/r4300/r4300_core.h"
#include "device/r4300/wasm_dynarec/wasm_dynarec.h"

/* minimal stubs to satisfy the dynarec build */
void DebugMessage(int level, const char *fmt, ...) {}
uint32_t *fast_mem_access(struct r4300_core *r4300, uint32_t address) { return NULL; }

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
    uint64_t regs[32];
    uint64_t hi;
    uint64_t lo;
    uint32_t pcaddr;
    uint32_t cp0[32];
    uint64_t cp1[32];
};

static void run_asm_test(const char *name, const uint32_t *code, size_t count,
                         const struct cpu_state *initial,
                         const struct cpu_state *expected)
{
    char wat_path[128];
    char wasm_path[128];
    char json_path[128];

    snprintf(wat_path, sizeof(wat_path),
             "mupen64plus-core/test/wasm_dynarec/%s.wat", name);
    snprintf(wasm_path, sizeof(wasm_path),
             "mupen64plus-core/test/wasm_dynarec/%s.wasm", name);
    snprintf(json_path, sizeof(json_path),
             "mupen64plus-core/test/wasm_dynarec/%s.json", name);

    struct r4300_core *cpu = calloc(1, sizeof(*cpu));
    ck_assert_ptr_nonnull(cpu);
    wasm_dynarec_init(cpu);
    wasm_dynarec_recompile_block(cpu, code, count, 0x80000000);
    const char *wat = wasm_dynarec_get_wat(0x80000000);
    ck_assert_ptr_nonnull(wat);

    FILE *f = fopen(wat_path, "w");
    ck_assert_ptr_nonnull(f);
    size_t len = strlen(wat);
    if (len > 2 && wat[len-2] == ')' && wat[len-1] == '\n')
        len -= 2;
    fwrite(wat, 1, len, f);
    fprintf(f, "  (export \"memory\" (memory 0))\n");
    fprintf(f, "  (export \"entry\" (func $block_80000000))\n)");
    fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "wat2wasm %s -o %s", wat_path, wasm_path);
    int ret = system(cmd);
    ck_assert_msg(ret == 0, "wat2wasm failed: %d", ret);

    FILE *json = fopen(json_path, "w");
    ck_assert_ptr_nonnull(json);
    fprintf(json, "{\n  \"initial\": {\"regs\": [");
    for (int i = 0; i < 32; i++) {
        fprintf(json, "%s%llu", i ? ", " : "", (unsigned long long)initial->regs[i]);
    }
    fprintf(json, "], \"hi\": %llu, \"lo\": %llu, \"pcaddr\": %u, \"cp0\": [",
            (unsigned long long)initial->hi,
            (unsigned long long)initial->lo,
            initial->pcaddr);
    for (int i = 0; i < 32; i++) {
        fprintf(json, "%s%u", i ? ", " : "", initial->cp0[i]);
    }
    fprintf(json, "], \"cp1\": [");
    for (int i = 0; i < 32; i++) {
        fprintf(json, "%s%llu", i ? ", " : "", (unsigned long long)initial->cp1[i]);
    }
    fprintf(json, "]},\n  \"expected\": {\"regs\": [");
    for (int i = 0; i < 32; i++) {
        fprintf(json, "%s%llu", i ? ", " : "", (unsigned long long)expected->regs[i]);
    }
    fprintf(json, "], \"hi\": %llu, \"lo\": %llu, \"pcaddr\": %u, \"cp0\": [",
            (unsigned long long)expected->hi,
            (unsigned long long)expected->lo,
            expected->pcaddr);
    for (int i = 0; i < 32; i++) {
        fprintf(json, "%s%u", i ? ", " : "", expected->cp0[i]);
    }
    fprintf(json, "], \"cp1\": [");
    for (int i = 0; i < 32; i++) {
        fprintf(json, "%s%llu", i ? ", " : "", (unsigned long long)expected->cp1[i]);
    }
    fprintf(json, "]}\n}\n");
    fclose(json);

    snprintf(cmd, sizeof(cmd),
             "node mupen64plus-core/test/wasm_dynarec/run_generated_wasm.js %s %s",
             wasm_path, json_path);
    ret = system(cmd);
    ck_assert_msg(ret == 0, "WebAssembly execution failed: %d", ret);

    wasm_dynarec_cleanup();
    free(cpu);
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

START_TEST(test_opcode_scenarios)
{
    const uint32_t arith_block[] = {
        0x20020003,
        0x20030005,
        0x00431020,
        0x00431822
    };
    struct cpu_state init = {0};
    struct cpu_state expect = {0};
    expect.regs[2] = 8;
    expect.regs[3] = 3;
    run_asm_test("arith", arith_block, sizeof(arith_block)/4, &init, &expect);

    const uint32_t branch_block[] = {
        0x20020000,
        0x10400001,
        0x20030001,
        0x20030002
    };
    memset(&init, 0, sizeof(init));
    memset(&expect, 0, sizeof(expect));
    expect.regs[3] = 2;
    run_asm_test("branch", branch_block, sizeof(branch_block)/4, &init, &expect);
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
    struct cpu_state init = {0};
    struct cpu_state expect = {0};
    expect.regs[8]  = 0xff00;
    expect.regs[9]  = 0xf0;
    expect.regs[10] = 0x0;
    expect.regs[11] = 0xfff0;
    expect.regs[12] = 0xfff0;
    expect.regs[13] = 0xffffffffffff000fULL;
    expect.regs[14] = 0xf00;
    expect.regs[15] = 0xff0;
    expect.regs[16] = 0xff0;
    run_asm_test("logic", logic_block, sizeof(logic_block)/4, &init, &expect);

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
    memset(&init, 0, sizeof(init));
    memset(&expect, 0, sizeof(expect));
    expect.regs[8]  = 10;
    expect.regs[9]  = 3;
    expect.regs[10] = 30;
    expect.regs[11] = 0;
    expect.regs[12] = 3;
    expect.regs[13] = 1;
    expect.hi = 1;
    expect.lo = 3;
    run_asm_test("muldiv", muldiv_block, sizeof(muldiv_block)/4, &init, &expect);

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
    memset(&init, 0, sizeof(init));
    memset(&expect, 0, sizeof(expect));
    expect.regs[8]  = (uint64_t)-10; /* t0 */
    expect.regs[9]  = 3;             /* t1 */
    expect.regs[10] = (uint64_t)-30; /* t2 */
    expect.regs[11] = (uint64_t)-1;  /* t3 */
    expect.regs[12] = (uint64_t)-3;  /* t4 */
    expect.regs[13] = (uint64_t)-1;  /* t5 */
    expect.hi = (uint64_t)-1;
    expect.lo = (uint64_t)-3;
    run_asm_test("muldiv_neg", muldiv_neg_block,
                 sizeof(muldiv_neg_block)/4, &init, &expect);

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
    memset(&init, 0, sizeof(init));
    memset(&expect, 0, sizeof(expect));
    expect.regs[8]  = 0x80000000ULL; /* t0 */
    expect.regs[9]  = 0x80000000ULL; /* t1 */
    expect.regs[10] = 4611686018427388000ULL; /* t2 */
    expect.regs[11] = 0x40000000ULL;          /* t3 */
    expect.hi = 0x40000000ULL;
    expect.lo = 4611686018427388000ULL;
    run_asm_test("mul_overflow", mult_overflow_block,
                 sizeof(mult_overflow_block)/4, &init, &expect);

    /* Divide by a negative value */
    const uint32_t div_neg_block[] = {
        0x6408000a, /* daddiu t0, zero, 10 */
        0x6409fffd, /* daddiu t1, zero, -3 */
        0x0109001a, /* div t0, t1 */
        0x00005012, /* mflo t2 */
        0x00005810  /* mfhi t3 */
    };
    memset(&init, 0, sizeof(init));
    memset(&expect, 0, sizeof(expect));
    expect.regs[8]  = 10;            /* t0 */
    expect.regs[9]  = (uint64_t)-3;  /* t1 */
    expect.regs[10] = (uint64_t)-3;  /* t2 quotient */
    expect.regs[11] = 1;             /* t3 remainder */
    expect.hi = 1;
    expect.lo = (uint64_t)-3;
    run_asm_test("div_neg", div_neg_block, sizeof(div_neg_block)/4, &init, &expect);

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
    memset(&init, 0, sizeof(init));
    memset(&expect, 0, sizeof(expect));
    expect.regs[8]  = 5;
    expect.regs[9]  = 10;
    expect.regs[10] = 1;
    expect.regs[11] = 1;
    expect.regs[12] = 0;
    expect.regs[13] = 0;
    expect.regs[14] = 1;
    expect.regs[15] = 0;
    run_asm_test("slt", slt_block, sizeof(slt_block)/4, &init, &expect);

    /* jal within the block */
    const uint32_t jal_block[] = {
        0x0c000004, /* jal 0x80000010 */
        0x20040001, /* addi a0, zero, 1 */
        0x20050002, /* addi a1, zero, 2 */
        0x20060003, /* addi a2, zero, 3 */
        0x03e00008, /* jr ra */
        0x00000000  /* nop */
    };
    memset(&init, 0, sizeof(init));
    memset(&expect, 0, sizeof(expect));
    expect.regs[4]  = 1;
    expect.regs[5]  = 2;
    expect.regs[6]  = 3;
    expect.regs[31] = 0xffffffff80000008ULL;
    expect.pcaddr   = 0x80000008;
    run_asm_test("jal", jal_block, sizeof(jal_block)/4, &init, &expect);
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
    struct cpu_state init = {0};
    struct cpu_state expect = {0};
    expect.regs[8] = 1;  /* t0 */
    expect.regs[9] = 2;  /* t1 */
    expect.regs[10] = 3; /* t2 */
    expect.regs[11] = 1; /* t3 */
    expect.regs[12] = 4; /* t4 */
    expect.regs[13] = 1; /* t5 */
    run_asm_test("unsigned", block, sizeof(block)/4, &init, &expect);
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
    struct cpu_state init = {0};
    struct cpu_state expect = {0};
    expect.regs[8]  = 0x2000;                 /* t0 */
    expect.regs[9]  = 0x1234567887654321ULL;  /* t1 */
    expect.regs[10] = 0x87654321;             /* t2 */
    expect.regs[11] = 0x5678;                 /* t3 */
    expect.regs[12] = 0x12345678;             /* t4 */
    expect.regs[13] = 0x78;                   /* t5 */
    expect.regs[14] = 0x5678;                 /* t6 */
    expect.regs[15] = 0x12345678;             /* t7 */
    expect.regs[16] = 0xffffffffffffffffULL;  /* s0 */
    expect.regs[17] = 0xff;                   /* s1 */
    expect.regs[18] = 0xffffffffffffffffULL;  /* s2 */
    expect.regs[19] = 0xffff;                 /* s3 */
    expect.regs[20] = 0xffffffffffffffffULL;  /* s4 */
    expect.regs[21] = 0xffffffff;             /* s5 */
    expect.regs[22] = 0x1234567887654321ULL;  /* s6 */
    run_asm_test("memory", mem_block, sizeof(mem_block)/4, &init, &expect);
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
    struct cpu_state init = {0};
    struct cpu_state expect = {0};
    expect.regs[8]  = 0x2000;      /* t0 */
    expect.regs[9]  = 0x89abcdef;  /* t1 */
    /* Values exceed 53-bit precision, use rounded JS Number equivalents */
    expect.regs[10] = 18446744073424413000ULL; /* t2 */
    expect.regs[11] = 18446744072869577000ULL; /* t3 */
    expect.regs[12] = 1;           /* t4 after sc */
    run_asm_test("unaligned", block, sizeof(block)/4, &init, &expect);
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
    struct cpu_state init = {0};
    struct cpu_state expect = {0};
    expect.regs[8] = 0;  /* t0 */
    expect.regs[9] = 15; /* t1 */
    run_asm_test("delay_beq_taken", taken_block, sizeof(taken_block)/4,
                 &init, &expect);

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
    memset(&init, 0, sizeof(init));
    memset(&expect, 0, sizeof(expect));
    expect.regs[8] = 1;  /* t0 */
    expect.regs[9] = 4;  /* t1 after delay slot and jump */
    run_asm_test("delay_bne_not", not_block, sizeof(not_block)/4, &init, &expect);
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
    struct cpu_state init = {0};
    struct cpu_state expect = {0};
    expect.regs[8]  = 1; /* t0 */
    expect.regs[9]  = 2; /* t1 */
    expect.regs[10] = 0; /* t2 remains 0 */
    run_asm_test("beql_skip", beql_block, sizeof(beql_block)/4, &init, &expect);

    /* BNEL - branch not taken skips delay slot */
    const uint32_t bnel_block[] = {
        0x20080001, /* addi t0, zero, 1 */
        0x20090001, /* addi t1, zero, 1 */
        0x55090002, /* bnel t0, t1, 2 */
        0x200a0005, /* addi t2, zero, 5 (delay slot) */
        0x00000000, /* nop */
        0x00000000  /* nop target */
    };
    memset(&init, 0, sizeof(init));
    memset(&expect, 0, sizeof(expect));
    expect.regs[8]  = 1; /* t0 */
    expect.regs[9]  = 1; /* t1 */
    expect.regs[10] = 0; /* t2 remains 0 */
    run_asm_test("bnel_skip", bnel_block, sizeof(bnel_block)/4, &init, &expect);

    /* BLEZL - branch not taken skips delay slot */
    const uint32_t blezl_block[] = {
        0x20080001, /* addi t0, zero, 1 */
        0x59000002, /* blezl t0, 2 */
        0x20090005, /* addi t1, zero, 5 (delay slot) */
        0x00000000, /* nop */
        0x00000000  /* nop target */
    };
    memset(&init, 0, sizeof(init));
    memset(&expect, 0, sizeof(expect));
    expect.regs[8] = 1; /* t0 */
    expect.regs[9] = 0; /* t1 remains 0 */
    run_asm_test("blezl_skip", blezl_block, sizeof(blezl_block)/4, &init, &expect);

    /* BGTZL - branch not taken skips delay slot */
    const uint32_t bgtzl_block[] = {
        0x20080000, /* addi t0, zero, 0 */
        0x5d000002, /* bgtzl t0, 2 */
        0x20090005, /* addi t1, zero, 5 (delay slot) */
        0x00000000, /* nop */
        0x00000000  /* nop target */
    };
    memset(&init, 0, sizeof(init));
    memset(&expect, 0, sizeof(expect));
    expect.regs[8] = 0; /* t0 */
    expect.regs[9] = 0; /* t1 remains 0 */
    run_asm_test("bgtzl_skip", bgtzl_block, sizeof(bgtzl_block)/4, &init, &expect);

    /* BLTZL - branch not taken skips delay slot */
    const uint32_t bltzl_block[] = {
        0x20080001, /* addi t0, zero, 1 */
        0x05020002, /* bltzl t0, 2 */
        0x20090005, /* addi t1, zero, 5 (delay slot) */
        0x00000000, /* nop */
        0x00000000  /* nop target */
    };
    memset(&init, 0, sizeof(init));
    memset(&expect, 0, sizeof(expect));
    expect.regs[8] = 1; /* t0 */
    expect.regs[9] = 0; /* t1 remains 0 */
    run_asm_test("bltzl_skip", bltzl_block, sizeof(bltzl_block)/4, &init, &expect);

    /* BGEZL - branch not taken skips delay slot */
    const uint32_t bgezl_block[] = {
        0x2008ffff, /* addi t0, zero, -1 */
        0x05030002, /* bgezl t0, 2 */
        0x20090005, /* addi t1, zero, 5 (delay slot) */
        0x00000000, /* nop */
        0x00000000  /* nop target */
    };
    memset(&init, 0, sizeof(init));
    memset(&expect, 0, sizeof(expect));
    expect.regs[8] = (uint64_t)-1; /* t0 */
    expect.regs[9] = 0;           /* t1 remains 0 */
    run_asm_test("bgezl_skip", bgezl_block, sizeof(bgezl_block)/4, &init, &expect);

    /* BGEZAL - branch taken updates link register */
    const uint32_t bgezal_block[] = {
        0x20080000, /* addi t0, zero, 0 */
        0x05110002, /* bgezal t0, 2 */
        0x20090005, /* addi t1, zero, 5 (delay slot) */
        0x00000000, /* nop */
        0x00000000  /* nop target */
    };
    memset(&init, 0, sizeof(init));
    memset(&expect, 0, sizeof(expect));
    expect.regs[8]  = 0;                      /* t0 */
    expect.regs[9]  = 5;                      /* t1 */
    expect.regs[31] = 0xffffffff8000000cULL;  /* ra */
    run_asm_test("bgezal_link", bgezal_block, sizeof(bgezal_block)/4, &init, &expect);

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
    struct cpu_state init = {0};
    struct cpu_state expect = {0};
    expect.regs[8]  = 0x12345678;
    expect.regs[9]  = 0x11112222;
    expect.regs[10] = 0x12345678;
    expect.regs[11] = 0x11112222;
    expect.cp0[0]   = 0x11112222;
    run_asm_test("cp0_moves", block, sizeof(block)/4, &init, &expect);
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
    struct cpu_state init = {0};
    struct cpu_state expect = {0};
    expect.regs[8]  = 0x12345678;      /* t0 */
    expect.regs[9]  = 0x12345678;      /* t1 */
    expect.regs[10] = 0x12345678;      /* t2 */
    expect.regs[11] = 0x2000;          /* t3 */
    expect.regs[14] = 0x12345678;      /* t6 */
    expect.regs[15] = 0x12345678;      /* t7 */
    for (int i = 0; i < 4; i++)
        expect.cp1[i] = 0x12345678ULL;
    run_asm_test("cp1_moves", block, sizeof(block)/4, &init, &expect);
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
    struct cpu_state init = {0};
    init.cp1[0]  = 0x3f800000ULL;           /* 1.0f */
    init.cp1[1]  = 0x40000000ULL;           /* 2.0f */
    init.cp1[10] = 0x3ff8000000000000ULL;   /* 1.5 */
    init.cp1[11] = 0x4000000000000000ULL;   /* 2.0 */
    struct cpu_state expect = init;
    expect.cp1[2]  = 0x40400000ULL;         /* 3.0f */
    expect.cp1[3]  = 0xbf800000ULL;         /* -1.0f */
    expect.cp1[4]  = 0x40000000ULL;         /* 2.0f */
    expect.cp1[5]  = 0x3f000000ULL;         /* 0.5f */
    expect.cp1[6]  = 0x3f800000ULL;         /* 1.0f */
    expect.cp1[7]  = 0xc0000000ULL;         /* -2.0f */
    expect.cp1[8]  = 0x3f000000ULL;         /* 0.5f */
    expect.cp1[12] = 0x400c000000000000ULL; /* 3.5 */
    expect.cp1[13] = 0xbfe0000000000000ULL; /* -0.5 */
    expect.cp1[14] = 0x4008000000000000ULL; /* 3.0 */
    expect.cp1[15] = 0x3fe8000000000000ULL; /* 0.75 */
    expect.cp1[16] = 0x3fe0000000000000ULL; /* 0.5 */
    expect.cp1[17] = 0xc008000000000000ULL; /* -3.0 */
    expect.cp1[18] = 0x3fe8000000000000ULL; /* 0.75 */
    run_asm_test("cp1_arith", block, sizeof(block)/4, &init, &expect);
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
    struct cpu_state init = {0};
    struct cpu_state expect = {0};
    expect.regs[8]  = 1;               /* t0 */
    expect.regs[9]  = 1;               /* t1 */
    expect.regs[10] = 5;               /* t2 */
    expect.regs[11] = 2;               /* t3 */
    expect.regs[12] = 3;               /* t4 */
    expect.regs[13] = 0x80000018;      /* t5 jump target */
    expect.pcaddr   = 0x80000018;      /* pcaddr after jr */
    run_asm_test("complex", block, sizeof(block)/4, &init, &expect);
}
END_TEST

Suite *create_suite(void)
{
    Suite *s = suite_create("WebAssembly Dynarec");
    TCase *tc_core = tcase_create("Core");
    tcase_add_test(tc_core, test_compile_example);
    tcase_add_test(tc_core, test_opcode_scenarios);
    tcase_add_test(tc_core, test_more_opcodes);
    tcase_add_test(tc_core, test_unsigned_ops);
    tcase_add_test(tc_core, test_memory_ops);
    tcase_add_test(tc_core, test_unaligned_ops);
    tcase_add_test(tc_core, test_delay_slots);
    tcase_add_test(tc_core, test_branch_likely);
    tcase_add_test(tc_core, test_cp0_moves);
    tcase_add_test(tc_core, test_cp1_moves);
    tcase_add_test(tc_core, test_cp1_arith);
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
