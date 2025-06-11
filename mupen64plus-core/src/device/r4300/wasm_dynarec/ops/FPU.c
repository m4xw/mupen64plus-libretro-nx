#include "../wasm_dynarec.h"
#include "../../new_dynarec/new_dynarec.h"

/*
 * Emit a subset of floating point instructions used by the WASM dynarec.
 * Only a handful of operations are supported for now. The helpers mirror the
 * older monolithic implementation but are simplified for clarity. Returns 1 if
 * the instruction was handled, 0 otherwise.
 */
int emit_fpu_instr(char **buf, size_t *size, uint32_t inst)
{
    uint32_t op = inst >> 26;
    if (op != 0x11)
        return 0; /* not a COP1 instruction */

    uint32_t fmt   = (inst >> 21) & 0x1f;
    uint32_t ft    = (inst >> 16) & 0x1f;
    uint32_t fs    = (inst >> 11) & 0x1f;
    uint32_t fd    = (inst >> 6)  & 0x1f;
    uint32_t funct = inst & 0x3f;

    switch (fmt) {
    case 0x10: /* single precision */
        switch (funct) {
        case 0x00: /* add.s */
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
        case 0x01: /* sub.s */
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
        case 0x02: /* mul.s */
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
        case 0x03: /* div.s */
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
        case 0x06: /* mov.s */
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
        case 0x07: /* neg.s */
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
        case 0x20: /* cvt.s.d */
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
        case 0x24: /* cvt.w.s */
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
        default:
            return 0;
        }
        return 1;

    case 0x11: /* double precision */
        switch (funct) {
        case 0x00: /* add.d */
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
        case 0x01: /* sub.d */
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
        case 0x02: /* mul.d */
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
        case 0x03: /* div.d */
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
        case 0x06: /* mov.d */
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
        case 0x07: /* neg.d */
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
        case 0x20: /* cvt.s.d handled in single section */
            return 0;
        case 0x21: /* cvt.d.s */
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
        default:
            return 0;
        }
        return 1;

    default:
        return 0;
    }
}
