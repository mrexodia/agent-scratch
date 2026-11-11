/* SPDX-License-Identifier: MIT */
#ifndef X87_INTERNAL_H
#define X87_INTERNAL_H

#include "x87.h"

#if !defined(__SIZEOF_INT128__)
#error "this library needs a compiler with unsigned __int128 (GCC, Clang)"
#endif
typedef unsigned __int128 u128;

#define IE X87_SW_IE
#define DE X87_SW_DE
#define ZE X87_SW_ZE
#define OE X87_SW_OE
#define UE X87_SW_UE
#define PE X87_SW_PE
#define SF X87_SW_SF

enum { RC_NEAR = 0, RC_DOWN = 1, RC_UP = 2, RC_ZERO = 3 };

/* Operand classes; everything >= K_QNAN is "NaN-like". */
enum { K_ZERO, K_DEN, K_NORM, K_INF, K_QNAN, K_SNAN, K_UNSUP };

/* Unpacked operand.  For finite non-zero values sig is normalised (bit 63
 * set) and exp is the biased exponent, which is < 1 for denormals.  For the
 * other classes sig/exp are the raw fields. */
typedef struct unp {
    uint8_t  kind, sign;
    int32_t  exp;
    uint64_t sig;
} unp;

/* Per-instruction arithmetic context. */
typedef struct fctx {
    uint16_t cw;
    uint16_t ex;    /* accumulated exception flags */
    int      c1;    /* last rounding was away from zero */
    int      abort; /* unmasked over/underflow on a memory store */
} fctx;

#define F80(se, m) ((x87_f80){ (m), (uint16_t)(se) })
#define F80_DEFAULT_NAN F80(0xFFFF, 0xC000000000000000ull)

static inline int clz64(uint64_t x) { return __builtin_clzll(x); }
static inline int ctx_rc(const fctx *c) { return (c->cw >> 10) & 3; }

unp x87i_unpack(x87_f80 v);
unp x87i_from_f32(uint32_t v);
unp x87i_from_f64(uint64_t v);
unp x87i_from_int(int64_t v);
x87_f80 x87i_pack_raw(const unp *a);

x87_f80 x87i_invalid(fctx *c);
x87_f80 x87i_nan1(fctx *c, const unp *a);
x87_f80 x87i_nan2(fctx *c, const unp *a, const unp *b);

/* Round (hi.lo * 2^(exp-16383-63)) to the precision in the control word. */
x87_f80 x87i_round_pack(fctx *c, int sign, int32_t exp, uint64_t hi, uint64_t lo);
/* Same, with explicit precision (24, 53 or 64 bits). */
x87_f80 x87i_round_pack_prec(fctx *c, int sign, int32_t exp, uint64_t hi, uint64_t lo, int bits);

x87_f80 x87i_add(fctx *c, const unp *a, const unp *b, int sub);
x87_f80 x87i_mul(fctx *c, const unp *a, const unp *b);
x87_f80 x87i_div(fctx *c, const unp *a, const unp *b);
x87_f80 x87i_sqrt(fctx *c, const unp *a);
x87_f80 x87i_rndint(fctx *c, const unp *a);

/* 0: a == b, 1: a > b, 2: a < b, 3: unordered */
int x87i_cmp(fctx *c, const unp *a, const unp *b, int quiet);

/* Convert to a signed integer of the given width; trunc selects chopping
 * instead of the current rounding mode. */
int64_t x87i_to_int(fctx *c, const unp *a, int bits, int trunc);
/* |a| rounded to an integer.  Returns non-zero if it does not fit 64 bits. */
int x87i_round_mag(int rc, const unp *a, uint64_t *mag, int *inexact, int *roundup);

uint32_t x87i_to_f32(fctx *c, x87_f80 v);
uint64_t x87i_to_f64(fctx *c, x87_f80 v);

/* Transcendentals (x87_trans.c).  Each returns the value for ST(0) (or the
 * popped destination). */
x87_f80 x87i_fsin(fctx *c, const unp *a, int *c2);
x87_f80 x87i_fcos(fctx *c, const unp *a, int *c2);
void    x87i_fsincos(fctx *c, const unp *a, x87_f80 *s, x87_f80 *co, int *c2);
x87_f80 x87i_fptan(fctx *c, const unp *a, int *c2);
x87_f80 x87i_fpatan(fctx *c, const unp *y, const unp *x);
x87_f80 x87i_f2xm1(fctx *c, const unp *a);
x87_f80 x87i_fyl2x(fctx *c, const unp *y, const unp *x);
x87_f80 x87i_fyl2xp1(fctx *c, const unp *y, const unp *x);

#endif
