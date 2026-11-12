/* SPDX-License-Identifier: MIT
 *
 * Portable smoke test of the value-level API (x87_ops.h): the way a lifter
 * that tracks the register stack itself would use the library.
 */
#include <stdio.h>
#include <string.h>
#include "x87_ops.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

static x87_f80 f80(uint16_t se, uint64_t m) { x87_f80 v = { m, se }; return v; }
static int same(x87_f80 a, uint16_t se, uint64_t m) { return a.sexp == se && a.mant == m; }

int main(void)
{
    x87_env e = { 0x037F, 0, 0, 0 };
    x87_f80 one = f80(0x3FFF, 0x8000000000000000ull), three = f80(0x4000, 0xC000000000000000ull), r, r2;
    x87_operand a, b;
    uint8_t bcd[10];
    uint16_t cc, mask;
    int c2;

    /* 1 / 3, round to nearest, 64-bit precision */
    a = x87_operand_f80(one); b = x87_operand_f80(three);
    r = x87_div(&a, &b, &e);
    CHECK(same(r, 0x3FFD, 0xAAAAAAAAAAAAAAABull) && e.flags == X87_SW_PE && e.c1 == 1 && !e.inhibit);
    /* same in double precision, rounding down */
    e.cw = 0x067F;
    r = x87_div(&a, &b, &e);
    CHECK(same(r, 0x3FFD, 0xAAAAAAAAAAAAA800ull) && e.flags == X87_SW_PE && e.c1 == 0);
    CHECK(x87_to_f64(r, &e) == 0x3FD5555555555555ull && e.flags == 0);

    /* 1 / 0: masked gives infinity, unmasked inhibits the result */
    e.cw = 0x037F;
    b = x87_operand_int(0);
    r = x87_div(&a, &b, &e);
    CHECK(same(r, 0x7FFF, 0x8000000000000000ull) && e.flags == X87_SW_ZE && !e.inhibit);
    e.cw = 0x037B;
    (void)x87_div(&a, &b, &e);
    CHECK(e.flags == X87_SW_ZE && e.inhibit);

    /* unmasked overflow delivers a bias-adjusted result */
    e.cw = 0x0377;
    a = x87_operand_f80(f80(0x7FFE, 0x8000000000000000ull));
    r = x87_mul(&a, &a, &e);
    CHECK(same(r, 0x7FFE + 0x3FFF - 24576, 0x8000000000000000ull) && e.flags == X87_SW_OE && !e.inhibit);

    /* memory operands: a signalling NaN float raises #IA and is quieted */
    e.cw = 0x037F;
    a = x87_operand_f80(one); b = x87_operand_f32(0x7F800001u);
    r = x87_add(&a, &b, &e);
    CHECK(same(r, 0x7FFF, 0xC000010000000000ull) && e.flags == X87_SW_IE);
    /* a denormal double raises #D both as an operand and when loaded */
    b = x87_operand_f64(1);
    (void)x87_add(&a, &b, &e);
    CHECK(e.flags == (X87_SW_DE | X87_SW_PE));
    r = x87_from_f64(1, &e);
    CHECK(same(r, 0x3FFF - 1074, 0x8000000000000000ull) && e.flags == X87_SW_DE && !e.inhibit);

    /* comparisons */
    a = x87_operand_f80(one); b = x87_operand_f80(three);
    CHECK(x87_cmp(&a, &b, 0, &e) == X87_CMP_LT && e.flags == 0);
    b = x87_operand_f80(f80(0x7FFF, 0xC000000000000000ull));
    CHECK(x87_cmp(&a, &b, 1, &e) == X87_CMP_UN && e.flags == 0);
    CHECK(x87_cmp(&a, &b, 0, &e) == X87_CMP_UN && e.flags == X87_SW_IE);
    CHECK(x87_xam(three) == X87_SW_C2 && x87_xam(f80(0, 0)) == X87_SW_C3);

    /* integer and BCD conversions */
    CHECK(x87_to_int(f80(0xC000, 0xA000000000000000ull), 32, 0, &e) == -2 && e.flags == X87_SW_PE); /* -2.5 -> -2 */
    CHECK(x87_to_int(f80(0x4000, 0xA000000000000000ull), 16, 1, &e) == 2);
    CHECK(x87_to_int(f80(0x403E, 0x8000000000000000ull), 32, 0, &e) == -2147483648ll && e.flags == X87_SW_IE);
    CHECK(same(x87_from_int(-5), 0xC001, 0xA000000000000000ull));
    x87_to_bcd(x87_from_int(-1234567), bcd, &e);
    CHECK(bcd[0] == 0x67 && bcd[1] == 0x45 && bcd[2] == 0x23 && bcd[3] == 0x01 && bcd[9] == 0x80 && e.flags == 0);
    CHECK(same(x87_from_bcd(bcd), x87_from_int(-1234567).sexp, x87_from_int(-1234567).mant));

    /* remainder, scale, extract */
    r = x87_prem(x87_from_int(17), x87_from_int(5), 0, &e, &cc, &mask);
    CHECK(same(r, 0x4000, 0x8000000000000000ull) && cc == (X87_SW_C3 | X87_SW_C1) && mask == 0x4700); /* 17 = 3*5 + 2 */
    r = x87_scale(three, x87_from_int(4), &e);
    CHECK(same(r, 0x4004, 0xC000000000000000ull));
    r = x87_xtract(x87_from_int(48), &r2, &e);
    CHECK(same(r, 0x4001, 0xA000000000000000ull) && same(r2, 0x3FFF, 0xC000000000000000ull)); /* 48 = 1.5 * 2^5 */

    /* constants follow the rounding mode */
    CHECK(same(x87_const(X87_CONST_PI, 0x037F), 0x4000, 0xC90FDAA22168C235ull));
    CHECK(same(x87_const(X87_CONST_PI, 0x0F7F), 0x4000, 0xC90FDAA22168C234ull));

    /* transcendentals */
    r = x87_sin(x87_const(X87_CONST_PI, 0x037F), &e, &c2); /* 66-bit pi: sin(pi) = -2^-64 */
    CHECK(same(r, 0xBFBF, 0x8000000000000000ull) && !c2);
    (void)x87_cos(f80(0x403F, 0x8000000000000000ull), &e, &c2);
    CHECK(c2 && e.inhibit);
    r = x87_yl2x(three, x87_from_int(8), &e); /* 3 * log2(8) */
    CHECK(same(r, 0x4002, 0x9000000000000000ull) && e.flags == X87_SW_PE);
    r = x87_atan2(one, one, &e);
    CHECK(same(r, 0x3FFE, 0xC90FDAA22168C235ull));
    r = x87_tan(f80(0, 0), &r2, &e, &c2);
    CHECK(same(r, 0, 0) && same(r2, 0x3FFF, 0x8000000000000000ull));
    r = x87_sqrt(x87_from_int(2), &e);
    CHECK(same(r, 0x3FFF, 0xB504F333F9DE6484ull) && e.flags == X87_SW_PE);

    /*
     * Places where the SDM text or Intel's formal model differs from what the
     * reference CPU does.  The library follows the CPU (each case was probed
     * on hardware); these pin that down.
     */
    /* FST m32 of an exactly representable float denormal with #U unmasked:
     * underflow is still signalled and nothing is stored */
    e.cw = 0x036F;
    (void)x87_to_f32(f80(0x3FFF - 130, 0x8000000000000000ull), &e);
    CHECK(e.flags == X87_SW_UE && e.inhibit);
    e.cw = 0x037F;
    CHECK(x87_to_f32(f80(0x3FFF - 130, 0x8000000000000000ull), &e) == 0x00080000u && e.flags == 0);
    /* tininess is detected after rounding: just below the smallest normal
     * float rounds up to it without #U */
    CHECK(x87_to_f32(f80(0x3FFF - 127, 0xFFFFFFFFFFFFFFFFull), &e) == 0x00800000u && e.flags == X87_SW_PE && e.c1 == 1);
    CHECK(x87_to_f64(f80(0x3FFF - 1023, 0xFFFFFFFFFFFFFFFFull), &e) == 0x0010000000000000ull && e.flags == X87_SW_PE);
    /* FIST of a denormal does not raise #D */
    CHECK(x87_to_int(f80(0, 0x1000), 32, 0, &e) == 0 && e.flags == X87_SW_PE);
    /* FSCALE of a denormal by zero returns it unchanged, even with #U unmasked */
    e.cw = 0x036F;
    r = x87_scale(f80(0, 0x1000), f80(0, 0), &e);
    CHECK(same(r, 0, 0x1000) && e.flags == X87_SW_DE && !e.inhibit);
    /* FPATAN(+inf, -inf) = 3pi/4 follows the rounding mode */
    e.cw = 0x037F;
    r = x87_atan2(f80(0x7FFF, 0x8000000000000000ull), f80(0xFFFF, 0x8000000000000000ull), &e);
    CHECK(same(r, 0x4000, 0x96CBE3F9990E91A8ull) && e.c1 == 1);
    e.cw = 0x0F7F;
    r = x87_atan2(f80(0x7FFF, 0x8000000000000000ull), f80(0xFFFF, 0x8000000000000000ull), &e);
    CHECK(same(r, 0x4000, 0x96CBE3F9990E91A7ull) && e.c1 == 0);
    /* a compare with an unmasked #IA still reports "unordered" */
    e.cw = 0x037E;
    a = x87_operand_f80(one); b = x87_operand_f80(f80(0x7FFF, 0xC000000000000000ull));
    CHECK(x87_cmp(&a, &b, 0, &e) == X87_CMP_UN && e.flags == X87_SW_IE && e.inhibit);
    /* a denormal divided by zero raises only #Z */
    e.cw = 0x037F;
    a = x87_operand_f80(f80(0, 0x1000)); b = x87_operand_int(0);
    (void)x87_div(&a, &b, &e);
    CHECK(e.flags == X87_SW_ZE);

    printf("ops_test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
