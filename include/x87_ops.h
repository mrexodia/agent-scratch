/* SPDX-License-Identifier: MIT
 *
 * x87 value-level operations.
 *
 * This is the layer underneath x87_exec(): pure functions that compute one
 * x87 operation on values, with exactly the hardware's results and exception
 * behaviour, but no register stack, no tags and no instruction decoding.  It
 * is meant for binary translators / lifters that track the x87 stack
 * themselves and only need the arithmetic.
 *
 * Protocol
 * --------
 * Every operation takes an x87_env.  The caller sets `cw` (the control word:
 * rounding mode, precision, exception masks).  The operation overwrites the
 * other fields:
 *
 *   flags    exception flags raised (X87_SW_IE .. X87_SW_PE); OR them into
 *            the status word.
 *   c1       value for the C1 condition code ("rounded up").
 *   inhibit  non-zero if an unmasked exception prevents the result from
 *            being delivered: the destination must be left unchanged and
 *            the stack must not be popped.  The returned value is then
 *            meaningless.  (An unmasked overflow/underflow/inexact does
 *            deliver a result to a register; the bias-adjusted value is
 *            what is returned.)
 *
 * With a constant, all-masked control word (0x037F) `inhibit` is always 0
 * and, once inlined, a compiler removes the checks; callers that do not
 * model exceptions can simply ignore everything but the return value.
 *
 * To get an inlinable build, compile src/x87_amalgam.c (to LLVM bitcode, for
 * instance) with -DX87_API='static inline' or an always_inline attribute.
 */
#ifndef X87_OPS_H
#define X87_OPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef X87_API
#define X87_API
#endif

/* 80-bit double extended value: 64-bit significand with explicit integer
 * bit, and sign + 15-bit biased exponent. */
typedef struct x87_f80 {
    uint64_t mant;
    uint16_t sexp;
} x87_f80;

/* Exception flags / status word bits */
#define X87_SW_IE  0x0001u
#define X87_SW_DE  0x0002u
#define X87_SW_ZE  0x0004u
#define X87_SW_OE  0x0008u
#define X87_SW_UE  0x0010u
#define X87_SW_PE  0x0020u
#define X87_SW_SF  0x0040u
#define X87_SW_ES  0x0080u
#define X87_SW_C0  0x0100u
#define X87_SW_C1  0x0200u
#define X87_SW_C2  0x0400u
#define X87_SW_C3  0x4000u
#define X87_SW_B   0x8000u

typedef struct x87_env {
    uint16_t cw;      /* in:  control word */
    uint16_t flags;   /* out: exceptions raised by the operation */
    int      c1;      /* out: C1 */
    int      inhibit; /* out: result must not be delivered */
} x87_env;

/*
 * An operand of an arithmetic operation.  Arithmetic with a memory operand
 * (FADD m32real, FIMUL m16int, ...) is not the same as converting first and
 * then operating on two registers - denormal and signalling NaN operands
 * are treated differently - so operands are built with the constructor that
 * matches their source.  Treat the contents as opaque.
 */
typedef struct x87_operand {
    uint8_t  kind, sign;
    int32_t  exp;
    uint64_t sig;
} x87_operand;

X87_API x87_operand x87_operand_f80(x87_f80 v);   /* a register / m80 value */
X87_API x87_operand x87_operand_f32(uint32_t bits);
X87_API x87_operand x87_operand_f64(uint64_t bits);
X87_API x87_operand x87_operand_int(int64_t v);   /* m16int / m32int */

/* ---- arithmetic: result goes to a register ---- */
X87_API x87_f80 x87_add(const x87_operand *a, const x87_operand *b, x87_env *e);  /* a + b */
X87_API x87_f80 x87_sub(const x87_operand *a, const x87_operand *b, x87_env *e);  /* a - b */
X87_API x87_f80 x87_mul(const x87_operand *a, const x87_operand *b, x87_env *e);
X87_API x87_f80 x87_div(const x87_operand *a, const x87_operand *b, x87_env *e);  /* a / b */
X87_API x87_f80 x87_sqrt(x87_f80 a, x87_env *e);
X87_API x87_f80 x87_rndint(x87_f80 a, x87_env *e);
X87_API x87_f80 x87_scale(x87_f80 a, x87_f80 b, x87_env *e);                      /* FSCALE: a * 2^trunc(b) */
/* FPREM (ieee = 0) / FPREM1 (ieee = 1) of a by b.  *cc receives the new
 * C0..C3 bits (as X87_SW_C* mask) and *cc_mask which of them are defined by
 * this outcome: sw = (sw & ~*cc_mask) | *cc. */
X87_API x87_f80 x87_prem(x87_f80 a, x87_f80 b, int ieee, x87_env *e, uint16_t *cc, uint16_t *cc_mask);
/* FXTRACT: returns the exponent (new ST(1)), stores the significand (new ST(0)). */
X87_API x87_f80 x87_xtract(x87_f80 a, x87_f80 *significand, x87_env *e);

/* ---- comparison ---- */
#define X87_CMP_EQ 0
#define X87_CMP_GT 1 /* a > b */
#define X87_CMP_LT 2
#define X87_CMP_UN 3 /* unordered */
/* quiet = 0: FCOM/FCOMI/FTST/FICOM (any NaN raises #IA);
 * quiet = 1: FUCOM/FUCOMI (only signalling NaNs do).
 * The relation is valid even when e->inhibit is set (the condition codes
 * are still written; only the pops are suppressed). */
X87_API int x87_cmp(const x87_operand *a, const x87_operand *b, int quiet, x87_env *e);
/* FXAM classification of a non-empty register, as a C0..C3 mask. */
X87_API uint16_t x87_xam(x87_f80 a);

/* ---- loads (FLD m32/m64, FILD, FBLD) ---- */
X87_API x87_f80 x87_from_f32(uint32_t bits, x87_env *e);
X87_API x87_f80 x87_from_f64(uint64_t bits, x87_env *e);
X87_API x87_f80 x87_from_int(int64_t v);          /* exact, never raises */
X87_API x87_f80 x87_from_bcd(const uint8_t bcd[10]);

/* ---- stores (FST m32/m64, FIST[T]P, FBSTP) ----
 * If e->inhibit is set nothing may be stored. */
X87_API uint32_t x87_to_f32(x87_f80 a, x87_env *e);
X87_API uint64_t x87_to_f64(x87_f80 a, x87_env *e);
/* bits = 16, 32 or 64; truncate = 1 for FISTTP.  The integer indefinite is
 * returned (and #IA raised) when the value does not fit. */
X87_API int64_t x87_to_int(x87_f80 a, int bits, int truncate, x87_env *e);
X87_API void x87_to_bcd(x87_f80 a, uint8_t bcd[10], x87_env *e);

/* ---- constants (FLD1, FLDL2T, FLDL2E, FLDPI, FLDLG2, FLDLN2, FLDZ) ----
 * The value depends on the rounding mode in cw. */
enum { X87_CONST_1, X87_CONST_L2T, X87_CONST_L2E, X87_CONST_PI, X87_CONST_LG2, X87_CONST_LN2, X87_CONST_Z };
X87_API x87_f80 x87_const(int which, uint16_t cw);

/* ---- transcendentals ----
 * The trigonometric functions set *out_of_range (C2) and deliver nothing
 * when |a| >= 2^63. */
X87_API x87_f80 x87_sin(x87_f80 a, x87_env *e, int *out_of_range);
X87_API x87_f80 x87_cos(x87_f80 a, x87_env *e, int *out_of_range);
X87_API x87_f80 x87_sincos(x87_f80 a, x87_f80 *cosine, x87_env *e, int *out_of_range); /* returns the sine */
/* FPTAN: returns the tangent; *one receives the value pushed afterwards
 * (1.0, or the NaN for a NaN/invalid operand). */
X87_API x87_f80 x87_tan(x87_f80 a, x87_f80 *one, x87_env *e, int *out_of_range);
X87_API x87_f80 x87_atan2(x87_f80 y, x87_f80 x, x87_env *e);  /* FPATAN:  y = ST(1), x = ST(0) */
X87_API x87_f80 x87_2xm1(x87_f80 a, x87_env *e);              /* F2XM1 */
X87_API x87_f80 x87_yl2x(x87_f80 y, x87_f80 x, x87_env *e);   /* FYL2X:   y = ST(1), x = ST(0) */
X87_API x87_f80 x87_yl2xp1(x87_f80 y, x87_f80 x, x87_env *e); /* FYL2XP1 */

#ifdef __cplusplus
}
#endif
#endif /* X87_OPS_H */
