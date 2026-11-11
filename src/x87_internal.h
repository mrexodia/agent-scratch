/* SPDX-License-Identifier: MIT */
#ifndef X87_INTERNAL_H
#define X87_INTERNAL_H

#include "x87.h"

/* Linkage of the library-internal functions.  The single translation unit
 * build (x87_amalgam.c) makes them static so that a compiler is free to
 * inline and specialise them into the public operations. */
#ifndef X87_INTERNAL
#define X87_INTERNAL
#endif

/* ------------------------------------------------------------------ */
/* 128-bit helpers                                                     */
/*                                                                     */
/* All wide arithmetic goes through a {hi, lo} pair.  Only the two     */
/* primitives below use unsigned __int128, and have a portable         */
/* replacement (define X87_NO_INT128 to force it).                     */
/* ------------------------------------------------------------------ */

typedef struct w128 {
    uint64_t hi, lo;
} w128;

#if defined(__SIZEOF_INT128__) && !defined(X87_NO_INT128)

/* a * b; returns the low half, stores the high half */
static inline uint64_t mul64(uint64_t a, uint64_t b, uint64_t *hi)
{
    unsigned __int128 p = (unsigned __int128)a * b;
    *hi = (uint64_t)(p >> 64);
    return (uint64_t)p;
}

/* (hi:lo) / d for hi < d; returns the quotient, stores the remainder */
static inline uint64_t div128(uint64_t hi, uint64_t lo, uint64_t d, uint64_t *rem)
{
    unsigned __int128 n = ((unsigned __int128)hi << 64) | lo;
    *rem = (uint64_t)(n % d);
    return (uint64_t)(n / d);
}

#else

static inline uint64_t mul64(uint64_t a, uint64_t b, uint64_t *hi)
{
    uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    *hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    return (mid << 32) | (uint32_t)ll;
}

static inline uint64_t div128(uint64_t hi, uint64_t lo, uint64_t d, uint64_t *rem)
{
    /* schoolbook division with 32-bit digits on a normalised divisor */
    int s = __builtin_clzll(d);
    uint64_t dh, dl, n32, n10, q1, q0, r, t;
    if (s) {
        d <<= s;
        hi = (hi << s) | (lo >> (64 - s));
        lo <<= s;
    }
    dh = d >> 32; dl = (uint32_t)d;
    n32 = hi; n10 = lo;

    q1 = n32 / dh; r = n32 % dh;
    while (q1 >> 32 || q1 * dl > ((r << 32) | (n10 >> 32))) {
        q1--; r += dh;
        if (r >> 32) break;
    }
    t = (n32 << 32) + (n10 >> 32) - q1 * d;

    q0 = t / dh; r = t % dh;
    while (q0 >> 32 || q0 * dl > ((r << 32) | (uint32_t)n10)) {
        q0--; r += dh;
        if (r >> 32) break;
    }
    *rem = ((t << 32) + (uint32_t)n10 - q0 * d) >> s;
    return (q1 << 32) | q0;
}

#endif

static inline w128 w_make(uint64_t hi, uint64_t lo) { w128 r = { hi, lo }; return r; }
static inline int w_is_zero(w128 a) { return !(a.hi | a.lo); }
static inline int w_cmp(w128 a, w128 b)
{
    if (a.hi != b.hi) return a.hi > b.hi ? 1 : -1;
    if (a.lo != b.lo) return a.lo > b.lo ? 1 : -1;
    return 0;
}
static inline w128 w_add(w128 a, w128 b, int *carry)
{
    w128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo);
    if (carry)
        *carry = r.hi < a.hi || (r.hi == a.hi && r.lo < a.lo);
    return r;
}
static inline w128 w_sub(w128 a, w128 b)
{
    w128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo);
    return r;
}
/* shifts by 0..127 */
static inline w128 w_shl(w128 a, int n)
{
    if (n >= 64) return w_make(a.lo << (n - 64), 0);
    if (n) return w_make((a.hi << n) | (a.lo >> (64 - n)), a.lo << n);
    return a;
}
static inline w128 w_shr(w128 a, int n)
{
    if (n >= 64) return w_make(0, a.hi >> (n - 64));
    if (n) return w_make(a.hi >> n, (a.lo >> n) | (a.hi << (64 - n)));
    return a;
}
/* right shift by any amount, ORing the lost bits into bit 0 ("jamming") */
static inline w128 w_shr_jam(w128 a, int n)
{
    w128 r;
    if (n <= 0) return a;
    if (n >= 128) return w_make(0, !w_is_zero(a));
    r = w_shr(a, n);
    if (!w_is_zero(w_shl(a, 128 - n)))
        r.lo |= 1;
    return r;
}

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

/* Unpacked operand (the public x87_operand).  For finite non-zero values
 * sig is normalised (bit 63 set) and exp is the biased exponent, which is
 * < 1 for denormals.  For the other classes sig/exp are the raw fields. */
typedef x87_operand unp;

/* Per-operation context (the public x87_env): cw in; flags, c1 and inhibit
 * out.  Inside the library inhibit is first set by a memory store with an
 * unmasked over/underflow and then finalised by x87i_resolve(). */
typedef x87_env fctx;

#define F80(se, m) ((x87_f80){ (m), (uint16_t)(se) })
#define F80_DEFAULT_NAN F80(0xFFFF, 0xC000000000000000ull)

static inline int clz64(uint64_t x) { return __builtin_clzll(x); }
static inline int ctx_rc(const fctx *c) { return (c->cw >> 10) & 3; }

X87_INTERNAL unp x87i_unpack(x87_f80 v);
X87_INTERNAL unp x87i_from_f32(uint32_t v);
X87_INTERNAL unp x87i_from_f64(uint64_t v);
X87_INTERNAL unp x87i_from_int(int64_t v);

X87_INTERNAL x87_f80 x87i_invalid(fctx *c);
X87_INTERNAL x87_f80 x87i_nan1(fctx *c, const unp *a);
X87_INTERNAL x87_f80 x87i_nan2(fctx *c, const unp *a, const unp *b);

/* Round (hi.lo * 2^(exp-16383-63)) to the precision in the control word. */
X87_INTERNAL x87_f80 x87i_round_pack(fctx *c, int sign, int32_t exp, uint64_t hi, uint64_t lo);
/* Same, with explicit precision (24, 53 or 64 bits). */
X87_INTERNAL x87_f80 x87i_round_pack_prec(fctx *c, int sign, int32_t exp, uint64_t hi, uint64_t lo, int bits);

X87_INTERNAL x87_f80 x87i_add(fctx *c, const unp *a, const unp *b, int sub);
X87_INTERNAL x87_f80 x87i_mul(fctx *c, const unp *a, const unp *b);
X87_INTERNAL x87_f80 x87i_div(fctx *c, const unp *a, const unp *b);
X87_INTERNAL x87_f80 x87i_sqrt(fctx *c, const unp *a);
X87_INTERNAL x87_f80 x87i_rndint(fctx *c, const unp *a);

/* 0: a == b, 1: a > b, 2: a < b, 3: unordered */
X87_INTERNAL int x87i_cmp(fctx *c, const unp *a, const unp *b, int quiet);

/* Convert to a signed integer of the given width; trunc selects chopping
 * instead of the current rounding mode. */
X87_INTERNAL int64_t x87i_to_int(fctx *c, const unp *a, int bits, int trunc);
X87_INTERNAL void x87i_to_bcd(fctx *c, const unp *a, uint8_t bcd[10]);
X87_INTERNAL x87_f80 x87i_from_bcd(const uint8_t bcd[10]);
X87_INTERNAL x87_f80 x87i_load(fctx *c, const unp *v);
X87_INTERNAL x87_f80 x87i_scale(fctx *c, x87_f80 a, x87_f80 b);
/* returns the remainder; *q = quotient, *status: 0 complete, 1 partial, 2 NaN/invalid */
X87_INTERNAL x87_f80 x87i_prem(fctx *c, x87_f80 a, x87_f80 b, int ieee, uint64_t *q, int *status);
X87_INTERNAL x87_f80 x87i_xtract(fctx *c, x87_f80 a, x87_f80 *sig);
X87_INTERNAL x87_f80 x87i_const(int which, uint16_t cw);
X87_INTERNAL uint16_t x87i_xam(x87_f80 v);

/* Decide whether an unmasked exception among `pre` (the pre-computation
 * faults that apply to the operation) or an aborted store inhibits the
 * result; if so reduce flags to what the hardware reports.  Returns
 * c->inhibit. */
X87_INTERNAL int x87i_resolve(fctx *c, uint16_t pre);
/* |a| rounded to an integer.  Returns non-zero if it does not fit 64 bits. */
X87_INTERNAL int x87i_round_mag(int rc, const unp *a, uint64_t *mag, int *inexact, int *roundup);

X87_INTERNAL uint32_t x87i_to_f32(fctx *c, x87_f80 v);
X87_INTERNAL uint64_t x87i_to_f64(fctx *c, x87_f80 v);

/* Transcendentals (x87_trans.c).  Each returns the value for ST(0) (or the
 * popped destination). */
X87_INTERNAL x87_f80 x87i_fsin(fctx *c, const unp *a, int *c2);
X87_INTERNAL x87_f80 x87i_fcos(fctx *c, const unp *a, int *c2);
X87_INTERNAL void    x87i_fsincos(fctx *c, const unp *a, x87_f80 *s, x87_f80 *co, int *c2);
X87_INTERNAL x87_f80 x87i_fptan(fctx *c, const unp *a, int *c2);
X87_INTERNAL x87_f80 x87i_fpatan(fctx *c, const unp *y, const unp *x);
X87_INTERNAL x87_f80 x87i_f2xm1(fctx *c, const unp *a);
X87_INTERNAL x87_f80 x87i_fyl2x(fctx *c, const unp *y, const unp *x);
X87_INTERNAL x87_f80 x87i_fyl2xp1(fctx *c, const unp *y, const unp *x);

#endif
