/* SPDX-License-Identifier: MIT
 *
 * Micro-benchmark: nanoseconds per operation for the value-level API and
 * for the instruction interpreter, on ordinary operands.
 *
 * usage: bench [iterations]
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "x87.h"

#define NV 1024
static x87_f80 va[NV], vb[NV];
static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static volatile uint64_t sink;
static long iters = 2000000;

#define BENCH(name, n, ...)                                              \
    do {                                                                 \
        long cnt = (n);                                                  \
        uint64_t acc = 0;                                                \
        double t0 = now(), t1;                                           \
        for (long i = 0; i < cnt; i++) {                                 \
            x87_f80 a = va[i & (NV - 1)], b = vb[i & (NV - 1)], r;       \
            (void)b;                                                     \
            __VA_ARGS__;                                                 \
            acc += r.mant;                                               \
        }                                                                \
        t1 = now();                                                      \
        sink = acc;                                                      \
        printf("  %-28s %8.1f ns\n", name, (t1 - t0) / (double)cnt);     \
    } while (0)

int main(int argc, char **argv)
{
    x87_env e = { 0x037F, 0, 0, 0 };
    x87_state s;
    x87_insn in = { 0 };
    int c2;
    x87_f80 t;

    if (argc > 1)
        iters = atol(argv[1]);
    for (int i = 0; i < NV; i++) {
        va[i].mant = rnd() | 0x8000000000000000ull; va[i].sexp = (uint16_t)(0x3FFF - 4 + rnd() % 8);
        vb[i].mant = rnd() | 0x8000000000000000ull; vb[i].sexp = (uint16_t)(0x3FFF - 4 + rnd() % 8);
    }

    printf("value-level API (x87_ops.h), 64-bit precision, round to nearest:\n");
    BENCH("add", iters, { x87_operand x = x87_operand_f80(a), y = x87_operand_f80(b); r = x87_add(&x, &y, &e); });
    BENCH("mul", iters, { x87_operand x = x87_operand_f80(a), y = x87_operand_f80(b); r = x87_mul(&x, &y, &e); });
    BENCH("div", iters, { x87_operand x = x87_operand_f80(a), y = x87_operand_f80(b); r = x87_div(&x, &y, &e); });
    BENCH("add m64real", iters, { x87_operand x = x87_operand_f80(a), y = x87_operand_f64(b.mant >> 2); r = x87_add(&x, &y, &e); });
    BENCH("sqrt", iters, r = x87_sqrt(a, &e));
    BENCH("rndint", iters, r = x87_rndint(a, &e));
    BENCH("compare", iters, { x87_operand x = x87_operand_f80(a), y = x87_operand_f80(b); r.mant = (uint64_t)x87_cmp(&x, &y, 0, &e); });
    BENCH("to double", iters, r.mant = x87_to_f64(a, &e));
    BENCH("from double", iters, r = x87_from_f64(a.mant >> 2, &e));
    BENCH("to int32", iters, r.mant = (uint64_t)x87_to_int(a, 32, 0, &e));
    BENCH("prem", iters, r = x87_prem(a, b, 0, &e, &(uint16_t){0}, &(uint16_t){0}));
    BENCH("sin", iters / 20, r = x87_sin(a, &e, &c2));
    BENCH("cos", iters / 20, r = x87_cos(a, &e, &c2));
    BENCH("sincos", iters / 20, r = x87_sincos(a, &t, &e, &c2));
    BENCH("tan", iters / 20, r = x87_tan(a, &t, &e, &c2));
    BENCH("atan2", iters / 20, r = x87_atan2(a, b, &e));
    BENCH("2xm1", iters / 20, { a.sexp = 0x3FFD; r = x87_2xm1(a, &e); });
    BENCH("yl2x", iters / 20, r = x87_yl2x(a, b, &e));
    BENCH("yl2xp1", iters / 20, { b.sexp = 0x3FFC; r = x87_yl2xp1(a, b, &e); });

    printf("interpreter (x87_exec):\n");
    x87_init(&s);
    {
        double d1 = 1.25, d2 = 3.5, out;
        double t0 = now(), t1;
        for (long i = 0; i < iters; i++) {
            in.op = 0xDD; in.modrm = 0x00; in.mem = &d1; x87_exec(&s, &in);  /* fld  m64 */
            in.op = 0xDC; in.modrm = 0x08; in.mem = &d2; x87_exec(&s, &in);  /* fmul m64 */
            in.op = 0xD8; in.modrm = 0xC0; x87_exec(&s, &in);                /* fadd st0,st0 */
            in.op = 0xDD; in.modrm = 0x18; in.mem = &out; x87_exec(&s, &in); /* fstp m64 */
        }
        t1 = now();
        sink = (uint64_t)out;
        printf("  %-28s %8.1f ns per instruction\n", "fld/fmul/fadd/fstp m64", (t1 - t0) / (double)iters / 4);
    }
    return 0;
}
