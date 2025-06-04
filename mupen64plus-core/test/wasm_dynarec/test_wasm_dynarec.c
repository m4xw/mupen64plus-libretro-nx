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

    /* Execution of the generated WebAssembly is not implemented yet. Normally
     * this test would run the module and compare CPU state using a helper
     * script. Until execution support exists we simply confirm that the WAT
     * assembles successfully. */

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
    expect.regs[13] = 0xffff000f;
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
    expect.regs[6]  = 3;
    expect.regs[31] = 0x80000008;
    run_asm_test("jal", jal_block, sizeof(jal_block)/4, &init, &expect);
}
END_TEST

Suite *create_suite(void)
{
    Suite *s = suite_create("WebAssembly Dynarec");
    TCase *tc_core = tcase_create("Core");
    tcase_add_test(tc_core, test_compile_example);
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
