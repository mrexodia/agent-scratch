/* SPDX-License-Identifier: MIT
 *
 * 80-bit extended precision arithmetic with x87 semantics: precision
 * control, the six exception flags, the C1 "rounded up" indication, x87 NaN
 * propagation rules and the unsupported/pseudo encodings.
 */
#include "x87_internal.h"

/* ------------------------------------------------------------------ */
/* Unpacking                                                           */
/* ------------------------------------------------------------------ */

X87_INTERNAL unp x87i_unpack(x87_f80 v)
{
    unp u;
    u.sign = v.sexp >> 15;
    u.exp = v.sexp & 0x7FFF;
    u.sig = v.mant;
    if (u.exp == 0x7FFF) {
        if (!(u.sig >> 63))
            u.kind = K_UNSUP; /* pseudo-infinity / pseudo-NaN */
        else if (!(u.sig << 1))
            u.kind = K_INF;
        else
            u.kind = (u.sig & 0x4000000000000000ull) ? K_QNAN : K_SNAN;
    } else if (u.exp == 0) {
        if (!u.sig) {
            u.kind = K_ZERO;
        } else {
            /* denormal or pseudo-denormal: true exponent is 1 */
            int s = clz64(u.sig);
            u.kind = K_DEN;
            u.sig <<= s;
            u.exp = 1 - s;
        }
    } else {
        u.kind = (u.sig >> 63) ? K_NORM : K_UNSUP; /* unnormal */
    }
    return u;
}

X87_INTERNAL unp x87i_from_f32(uint32_t v)
{
    unp u;
    uint32_t e = (v >> 23) & 0xFF, f = v & 0x7FFFFF;
    u.sign = v >> 31;
    if (e == 0xFF) {
        u.exp = 0x7FFF;
        u.sig = 0x8000000000000000ull | ((uint64_t)f << 40);
        u.kind = !f ? K_INF : (f & 0x400000) ? K_QNAN : K_SNAN;
    } else if (e == 0) {
        if (!f) {
            u.kind = K_ZERO; u.exp = 0; u.sig = 0;
        } else {
            int s = clz64(f);
            u.kind = K_DEN;
            u.sig = (uint64_t)f << s;
            u.exp = 16383 - 126 - (s - 40);
        }
    } else {
        u.kind = K_NORM;
        u.exp = (int32_t)e - 127 + 16383;
        u.sig = 0x8000000000000000ull | ((uint64_t)f << 40);
    }
    return u;
}

X87_INTERNAL unp x87i_from_f64(uint64_t v)
{
    unp u;
    uint32_t e = (v >> 52) & 0x7FF;
    uint64_t f = v & 0xFFFFFFFFFFFFFull;
    u.sign = v >> 63;
    if (e == 0x7FF) {
        u.exp = 0x7FFF;
        u.sig = 0x8000000000000000ull | (f << 11);
        u.kind = !f ? K_INF : (f & 0x8000000000000ull) ? K_QNAN : K_SNAN;
    } else if (e == 0) {
        if (!f) {
            u.kind = K_ZERO; u.exp = 0; u.sig = 0;
        } else {
            int s = clz64(f);
            u.kind = K_DEN;
            u.sig = f << s;
            u.exp = 16383 - 1022 - (s - 11);
        }
    } else {
        u.kind = K_NORM;
        u.exp = (int32_t)e - 1023 + 16383;
        u.sig = 0x8000000000000000ull | (f << 11);
    }
    return u;
}

X87_INTERNAL unp x87i_from_int(int64_t v)
{
    unp u;
    uint64_t m = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    u.sign = v < 0;
    if (!m) {
        u.kind = K_ZERO; u.exp = 0; u.sig = 0;
    } else {
        int s = clz64(m);
        u.kind = K_NORM;
        u.sig = m << s;
        u.exp = 16383 + 63 - s;
    }
    return u;
}

/* ------------------------------------------------------------------ */
/* NaN handling                                                        */
/* ------------------------------------------------------------------ */

X87_INTERNAL x87_f80 x87i_invalid(fctx *c)
{
    /* an invalid operation pre-empts the denormal operand exception */
    c->flags = (uint16_t)((c->flags & ~DE) | IE);
    return F80_DEFAULT_NAN;
}

X87_INTERNAL x87_f80 x87i_nan1(fctx *c, const unp *a)
{
    if (a->kind == K_UNSUP)
        return x87i_invalid(c);
    if (a->kind == K_SNAN)
        c->flags |= IE;
    return F80((a->sign << 15) | 0x7FFF, a->sig | 0x4000000000000000ull);
}

X87_INTERNAL x87_f80 x87i_nan2(fctx *c, const unp *a, const unp *b)
{
    const unp *r;
    int an = a->kind >= K_QNAN, bn = b->kind >= K_QNAN;

    if (a->kind == K_UNSUP || b->kind == K_UNSUP)
        return x87i_invalid(c);
    if (a->kind == K_SNAN || b->kind == K_SNAN)
        c->flags |= IE;
    if (!bn)
        r = a;
    else if (!an)
        r = b;
    else if (a->kind != b->kind)
        r = a->kind == K_QNAN ? a : b; /* SNaN vs QNaN: the QNaN wins */
    else if (a->sig != b->sig)
        r = a->sig > b->sig ? a : b;   /* larger significand */
    else
        r = a->sign ? b : a;           /* equal: the positive one */
    return F80((r->sign << 15) | 0x7FFF, r->sig | 0x4000000000000000ull);
}

/* ------------------------------------------------------------------ */
/* Rounding                                                            */
/* ------------------------------------------------------------------ */

static inline int round_decide(int rc, int sign, uint64_t hi, uint64_t lo, uint64_t unit, int *inexact)
{
    uint64_t fmask = unit - 1, r = hi & fmask;
    int guard, sticky;
    if (unit == 1) {
        guard = lo >> 63;
        sticky = (lo << 1) != 0;
    } else {
        uint64_t half = unit >> 1;
        guard = (r & half) != 0;
        sticky = ((r & (half - 1)) | lo) != 0;
    }
    *inexact = guard | sticky;
    switch (rc) {
    case RC_NEAR: return guard && (sticky || (hi & unit));
    case RC_DOWN: return sign && *inexact;
    case RC_UP:   return !sign && *inexact;
    default:      return 0;
    }
}

/*
 * Core rounding.  value = hi.lo * 2^(exp - bias - 63), hi normalised.
 * unit   - weight of the result's least significant bit inside hi
 * expinf - the all-ones exponent of the destination format
 * adj    - exponent adjustment for unmasked over/underflow delivered to a
 *          register (24576); 0 means "abort" (memory destination).
 * Returns 1 if the result is an infinity.  On return *pexp == 0 denotes a
 * denormal (or zero) result.
 */
static int round_core(fctx *c, int sign, int32_t *pexp, uint64_t *phi, uint64_t lo,
                      uint64_t unit, int32_t expinf, int32_t adj)
{
    uint64_t hi = *phi, fmask = unit - 1;
    int32_t exp = *pexp;
    int rc = ctx_rc(c), inexact, up;

    if (exp <= 0) {
        /* tininess is detected after rounding */
        int tiny = 1;
        if (exp == 0) {
            up = round_decide(rc, sign, hi, lo, unit, &inexact);
            if (up && (hi | fmask) == ~0ull)
                tiny = 0;
        }
        if (tiny) {
            if (!(c->cw & UE) && (!adj || exp + adj > 0)) {
                c->flags |= UE;
                if (!adj) {
                    c->inhibit = 1;
                    return 0;
                }
                exp += adj;
            } else {
                /* masked response (also used when the result is still
                 * out of range after the bias adjustment, as with FSCALE) */
                w128 v = w_shr_jam(w_make(hi, lo), 1 - exp);
                hi = v.hi;
                lo = v.lo;
                up = round_decide(rc, sign, hi, lo, unit, &inexact);
                hi &= ~fmask;
                if (up)
                    hi += unit;
                if (inexact)
                    c->flags |= UE | PE;
                else if (!(c->cw & UE))
                    c->flags |= UE;
                c->c1 = up;
                *phi = hi;
                *pexp = (hi >> 63) ? 1 : 0;
                return 0;
            }
        }
    }

    up = round_decide(rc, sign, hi, lo, unit, &inexact);
    hi &= ~fmask;
    if (up) {
        hi += unit;
        if (!hi) {
            hi = 0x8000000000000000ull;
            exp++;
        }
    }
    if (exp >= expinf) {
        c->flags |= OE;
        if (!(c->cw & OE) && (!adj || exp - adj < expinf)) {
            if (!adj) {
                c->inhibit = 1;
                return 0;
            }
            exp -= adj;
            if (inexact)
                c->flags |= PE;
            c->c1 = up;
        } else {
            int toinf = rc == RC_NEAR || (rc == RC_UP && !sign) || (rc == RC_DOWN && sign);
            c->flags |= PE;
            c->c1 = toinf;
            if (toinf) {
                *phi = 0x8000000000000000ull;
                *pexp = expinf;
                return 1;
            }
            hi = ~fmask;
            exp = expinf - 1;
        }
    } else {
        if (inexact)
            c->flags |= PE;
        c->c1 = up;
    }
    *phi = hi;
    *pexp = exp;
    return 0;
}

X87_INTERNAL x87_f80 x87i_round_pack_prec(fctx *c, int sign, int32_t exp, uint64_t hi, uint64_t lo, int bits)
{
    uint64_t unit = bits == 24 ? 1ull << 40 : bits == 53 ? 1ull << 11 : 1;
    round_core(c, sign, &exp, &hi, lo, unit, 0x7FFF, 24576);
    return F80((sign << 15) | (exp & 0x7FFF), hi);
}

X87_INTERNAL x87_f80 x87i_round_pack(fctx *c, int sign, int32_t exp, uint64_t hi, uint64_t lo)
{
    static const uint8_t bits[4] = { 24, 64, 53, 64 };
    return x87i_round_pack_prec(c, sign, exp, hi, lo, bits[(c->cw >> 8) & 3]);
}

/* ------------------------------------------------------------------ */
/* Basic arithmetic                                                    */
/* ------------------------------------------------------------------ */

static inline x87_f80 mk_inf(int sign) { return F80((sign << 15) | 0x7FFF, 0x8000000000000000ull); }
static inline x87_f80 mk_zero(int sign) { return F80(sign << 15, 0); }

X87_INTERNAL x87_f80 x87i_add(fctx *c, const unp *a, const unp *b, int sub)
{
    int sa = a->sign, sb = b->sign ^ sub;
    int32_t ea, eb, d;
    w128 x, y, s;

    if (a->kind >= K_QNAN || b->kind >= K_QNAN)
        return x87i_nan2(c, a, b);
    if (a->kind == K_INF || b->kind == K_INF) {
        if (a->kind == K_INF && b->kind == K_INF && sa != sb)
            return x87i_invalid(c);
        if (a->kind == K_DEN || b->kind == K_DEN)
            c->flags |= DE;
        return mk_inf(a->kind == K_INF ? sa : sb);
    }
    if (a->kind == K_DEN || b->kind == K_DEN)
        c->flags |= DE;
    if (b->kind == K_ZERO) {
        if (a->kind == K_ZERO)
            return mk_zero(sa == sb ? sa : ctx_rc(c) == RC_DOWN);
        return x87i_round_pack(c, sa, a->exp, a->sig, 0);
    }
    if (a->kind == K_ZERO)
        return x87i_round_pack(c, sb, b->exp, b->sig, 0);

    ea = a->exp; eb = b->exp;
    x = w_make(a->sig, 0);
    y = w_make(b->sig, 0);
    if (ea < eb || (ea == eb && x.hi < y.hi)) {
        w128 t = x; x = y; y = t;
        int32_t te = ea; ea = eb; eb = te;
        int ts = sa; sa = sb; sb = ts;
    }
    d = ea - eb;
    y = w_shr_jam(y, d);
    if (sa == sb) {
        int carry;
        s = w_add(x, y, &carry);
        if (carry) {
            s = w_make((s.hi >> 1) | 0x8000000000000000ull, (s.lo >> 1) | (s.hi << 63) | (s.lo & 1));
            ea++;
        }
    } else {
        int sh;
        s = w_sub(x, y);
        if (w_is_zero(s))
            return mk_zero(ctx_rc(c) == RC_DOWN);
        sh = s.hi ? clz64(s.hi) : 64 + clz64(s.lo);
        s = w_shl(s, sh);
        ea -= sh;
    }
    return x87i_round_pack(c, sa, ea, s.hi, s.lo);
}

X87_INTERNAL x87_f80 x87i_mul(fctx *c, const unp *a, const unp *b)
{
    int sign = a->sign ^ b->sign;
    int32_t exp;
    uint64_t hi, lo;

    if (a->kind >= K_QNAN || b->kind >= K_QNAN)
        return x87i_nan2(c, a, b);
    if (a->kind == K_INF || b->kind == K_INF) {
        if (a->kind == K_ZERO || b->kind == K_ZERO)
            return x87i_invalid(c);
        if (a->kind == K_DEN || b->kind == K_DEN)
            c->flags |= DE;
        return mk_inf(sign);
    }
    if (a->kind == K_DEN || b->kind == K_DEN)
        c->flags |= DE;
    if (a->kind == K_ZERO || b->kind == K_ZERO)
        return mk_zero(sign);

    lo = mul64(a->sig, b->sig, &hi);
    exp = a->exp + b->exp - 16383 + 1;
    if (!(hi >> 63)) {
        hi = (hi << 1) | (lo >> 63);
        lo <<= 1;
        exp--;
    }
    return x87i_round_pack(c, sign, exp, hi, lo);
}

X87_INTERNAL x87_f80 x87i_div(fctx *c, const unp *a, const unp *b)
{
    int sign = a->sign ^ b->sign;
    int32_t exp;
    uint64_t q, q2, r;

    if (a->kind >= K_QNAN || b->kind >= K_QNAN)
        return x87i_nan2(c, a, b);
    if (a->kind == K_INF) {
        if (b->kind == K_INF)
            return x87i_invalid(c);
        if (b->kind == K_DEN)
            c->flags |= DE;
        return mk_inf(sign);
    }
    if (b->kind == K_INF) {
        if (a->kind == K_DEN)
            c->flags |= DE;
        return mk_zero(sign);
    }
    if (b->kind == K_ZERO) {
        if (a->kind == K_ZERO)
            return x87i_invalid(c);
        c->flags |= ZE; /* no #D for a denormal dividend here */
        return mk_inf(sign);
    }
    if (a->kind == K_DEN || b->kind == K_DEN)
        c->flags |= DE;
    if (a->kind == K_ZERO)
        return mk_zero(sign);

    exp = a->exp - b->exp + 16383;
    if (a->sig >= b->sig) {
        q = div128(a->sig >> 1, a->sig << 63, b->sig, &r);
    } else {
        q = div128(a->sig, 0, b->sig, &r);
        exp--;
    }
    q2 = div128(r, 0, b->sig, &r);
    if (r)
        q2 |= 1;
    return x87i_round_pack(c, sign, exp, q, q2);
}

/* floor(sqrt(v)) for v >= 2^62; the root is in [2^31, 2^32) */
static uint64_t isqrt64(uint64_t v)
{
    /* Newton's iteration from above converges monotonically */
    uint64_t x = 0xFFFFFFFFull, y;
    for (;;) {
        y = (x + v / x) >> 1;
        if (y >= x)
            return x;
        x = y;
    }
}

X87_INTERNAL x87_f80 x87i_sqrt(fctx *c, const unp *a)
{
    uint64_t hi, lo, s1, r1, q, u, s, a1, a0, rem_lo, q2_hi, q2_lo;
    w128 num, rem, q2;
    int32_t e;

    if (a->kind >= K_QNAN)
        return x87i_nan1(c, a);
    if (a->kind == K_ZERO)
        return mk_zero(a->sign);
    if (a->sign)
        return x87i_invalid(c);
    if (a->kind == K_INF)
        return mk_inf(0);
    if (a->kind == K_DEN)
        c->flags |= DE;

    /* value = n * 2^(e - 126) with e even and n = hi:lo in [2^126, 2^128) */
    e = a->exp - 16383;
    if (e & 1) {
        hi = a->sig; lo = 0;
        e -= 1;
    } else {
        hi = a->sig >> 1; lo = a->sig << 63;
    }
    /*
     * Integer square root with remainder of the 128-bit n, by one
     * "Karatsuba" step (Zimmermann): take the root s1 of the top half,
     * then divide to get the low half of the root.
     */
    a1 = lo >> 32; a0 = (uint32_t)lo;
    s1 = isqrt64(hi);
    r1 = hi - s1 * s1;                      /* <= 2*s1 */
    num = w_make(r1 >> 32, (r1 << 32) | a1); /* r1 * 2^32 + a1 */
    q = div128(num.hi, num.lo, 2 * s1, &u);
    s = (s1 << 32) + q;
    /* remainder = u * 2^32 + a0 - q^2 (may be negative: then s is one too big) */
    rem = w_make(u >> 32, (u << 32) | a0);
    q2_lo = mul64(q, q, &q2_hi);
    q2 = w_make(q2_hi, q2_lo);
    if (w_cmp(rem, q2) < 0) {
        s--;
        rem = w_add(rem, w_make(s >> 63, (s << 1) | 1), 0); /* + 2s + 1 */
    }
    rem = w_sub(rem, q2);
    /* s = floor(sqrt(n)), rem = n - s^2 <= 2s.  sqrt(n) > s + 1/2 iff rem > s */
    rem_lo = (rem.hi || rem.lo > s) ? 0xC000000000000000ull : rem.lo ? 0x4000000000000000ull : 0;
    return x87i_round_pack(c, 0, (e >> 1) + 16383, s, rem_lo);
}

/* ------------------------------------------------------------------ */
/* Integer rounding and conversions                                    */
/* ------------------------------------------------------------------ */

X87_INTERNAL int x87i_round_mag(int rc, const unp *a, uint64_t *mag, int *inexact, int *roundup)
{
    int32_t e = a->exp - 16383;
    uint64_t ip, frac;
    int guard, sticky, up;

    *inexact = 0; *roundup = 0;
    if (a->kind == K_ZERO) {
        *mag = 0;
        return 0;
    }
    if (e >= 64)
        return 1;
    if (e == 63) {
        *mag = a->sig;
        return 0;
    }
    if (e < -1) {
        ip = 0; guard = 0; sticky = 1;
    } else if (e == -1) {
        ip = 0; guard = 1; sticky = (a->sig << 1) != 0;
    } else {
        int sh = 63 - e;
        ip = a->sig >> sh;
        frac = a->sig << (64 - sh);
        guard = frac >> 63;
        sticky = (frac << 1) != 0;
    }
    *inexact = guard | sticky;
    switch (rc) {
    case RC_NEAR: up = guard && (sticky || (ip & 1)); break;
    case RC_DOWN: up = a->sign && *inexact; break;
    case RC_UP:   up = !a->sign && *inexact; break;
    default:      up = 0; break;
    }
    if (up) {
        ip++;
        if (!ip)
            return 1;
    }
    *roundup = up;
    *mag = ip;
    return 0;
}

X87_INTERNAL int64_t x87i_to_int(fctx *c, const unp *a, int bits, int trunc)
{
    uint64_t mag, lim = 1ull << (bits - 1);
    int64_t indef = (int64_t)(0 - lim);
    int inexact, up;

    if (a->kind >= K_INF) {
        c->flags |= IE;
        return indef;
    }
    if (x87i_round_mag(trunc ? RC_ZERO : ctx_rc(c), a, &mag, &inexact, &up) ||
        mag > lim || (mag == lim && !a->sign)) {
        c->flags |= IE;
        return indef;
    }
    if (inexact)
        c->flags |= PE;
    c->c1 = up;
    return a->sign ? (int64_t)(0 - mag) : (int64_t)mag;
}

X87_INTERNAL x87_f80 x87i_rndint(fctx *c, const unp *a)
{
    uint64_t mag;
    int inexact, up, s;

    if (a->kind >= K_QNAN)
        return x87i_nan1(c, a);
    if (a->kind == K_INF)
        return mk_inf(a->sign);
    if (a->kind == K_ZERO)
        return mk_zero(a->sign);
    if (a->kind == K_DEN)
        c->flags |= DE;
    if (a->exp >= 16383 + 63)
        return F80((a->sign << 15) | a->exp, a->sig);
    x87i_round_mag(ctx_rc(c), a, &mag, &inexact, &up);
    if (inexact)
        c->flags |= PE;
    c->c1 = up;
    if (!mag)
        return mk_zero(a->sign);
    s = clz64(mag);
    return F80((a->sign << 15) | (16383 + 63 - s), mag << s);
}

static uint64_t to_float(fctx *c, x87_f80 v, uint64_t unit, int fbits, int32_t bias, int32_t expinf)
{
    unp a = x87i_unpack(v);
    uint64_t sign = (uint64_t)a.sign << (fbits + (expinf == 0xFF ? 8 : 11));
    uint64_t einf = (uint64_t)expinf << fbits;
    uint64_t hi;
    int32_t exp;

    switch (a.kind) {
    case K_UNSUP:
        c->flags |= IE;
        return ((uint64_t)1 << (fbits + (expinf == 0xFF ? 8 : 11))) | einf | (1ull << (fbits - 1));
    case K_SNAN:
        c->flags |= IE;
        /* fall through */
    case K_QNAN:
        return sign | einf | (1ull << (fbits - 1)) | ((a.sig << 1) >> (64 - fbits));
    case K_INF:
        return sign | einf;
    case K_ZERO:
        return sign;
    default:
        break;
    }
    hi = a.sig;
    exp = a.exp - 16383 + bias;
    if (round_core(c, a.sign, &exp, &hi, 0, unit, expinf, 0))
        return sign | einf;
    if (c->inhibit)
        return 0;
    return sign | ((uint64_t)exp << fbits) | ((hi << 1) >> (64 - fbits));
}

X87_INTERNAL uint32_t x87i_to_f32(fctx *c, x87_f80 v)
{
    return (uint32_t)to_float(c, v, 1ull << 40, 23, 127, 0xFF);
}

X87_INTERNAL uint64_t x87i_to_f64(fctx *c, x87_f80 v)
{
    return to_float(c, v, 1ull << 11, 52, 1023, 0x7FF);
}

/* ------------------------------------------------------------------ */
/* Comparison                                                          */
/* ------------------------------------------------------------------ */

X87_INTERNAL int x87i_cmp(fctx *c, const unp *a, const unp *b, int quiet)
{
    int r;
    if (a->kind >= K_QNAN || b->kind >= K_QNAN) {
        if (!quiet || a->kind >= K_SNAN || b->kind >= K_SNAN)
            c->flags |= IE;
        return 3;
    }
    if (a->kind == K_DEN || b->kind == K_DEN)
        c->flags |= DE;
    if (a->kind == K_ZERO && b->kind == K_ZERO)
        return 0;
    if (a->kind == K_ZERO)
        return b->sign ? 1 : 2;
    if (b->kind == K_ZERO)
        return a->sign ? 2 : 1;
    if (a->sign != b->sign)
        return a->sign ? 2 : 1;
    /* same sign, both non-zero: compare magnitudes */
    if (a->kind == K_INF || b->kind == K_INF) {
        if (a->kind == b->kind)
            return 0;
        r = a->kind == K_INF ? 1 : 2;
    } else if (a->exp != b->exp) {
        r = a->exp > b->exp ? 1 : 2;
    } else if (a->sig != b->sig) {
        r = a->sig > b->sig ? 1 : 2;
    } else {
        return 0;
    }
    return a->sign ? 3 - r : r;
}

/* ------------------------------------------------------------------ */
/* Exception resolution                                                */
/* ------------------------------------------------------------------ */

X87_INTERNAL int x87i_resolve(fctx *c, uint16_t pre)
{
    uint16_t f = c->flags & pre;
    if (f & ~c->cw) {
        /* an unmasked pre-computation fault: nothing else is reported */
        c->flags = f;
        c->c1 = 0;
        c->inhibit = 1;
    } else if (c->inhibit) {
        c->c1 = 0; /* store abandoned on unmasked over/underflow */
    }
    return c->inhibit;
}

/* ------------------------------------------------------------------ */
/* Loads, BCD, constants, classification                               */
/* ------------------------------------------------------------------ */

/* FLD m32real/m64real: the widened operand as a register value. */
X87_INTERNAL x87_f80 x87i_load(fctx *c, const unp *v)
{
    if (v->kind >= K_QNAN)
        return x87i_nan1(c, v);
    if (v->kind == K_INF)
        return mk_inf(v->sign);
    if (v->kind == K_ZERO)
        return mk_zero(v->sign);
    if (v->kind == K_DEN)
        c->flags |= DE;
    return F80((v->sign << 15) | v->exp, v->sig);
}

X87_INTERNAL x87_f80 x87i_from_bcd(const uint8_t p[10])
{
    uint64_t v = 0;
    unp u;
    /* invalid digits are not rejected; they simply contribute digit * 10^n */
    for (int i = 8; i >= 0; i--)
        v = v * 100 + (uint64_t)(p[i] >> 4) * 10 + (p[i] & 15);
    u = x87i_from_int((int64_t)v);
    if (u.kind == K_ZERO)
        return F80((p[9] & 0x80) << 8, 0);
    return F80(((p[9] & 0x80) << 8) | u.exp, u.sig);
}

X87_INTERNAL void x87i_to_bcd(fctx *c, const unp *a, uint8_t bcd[10])
{
    uint64_t mag = 0;
    int inexact = 0, up = 0;
    if (a->kind >= K_INF || x87i_round_mag(ctx_rc(c), a, &mag, &inexact, &up) ||
        mag > 999999999999999999ull) {
        /* packed BCD indefinite */
        c->flags |= IE;
        for (int i = 0; i < 7; i++)
            bcd[i] = 0;
        bcd[7] = 0xC0;
        bcd[8] = bcd[9] = 0xFF;
        return;
    }
    if (inexact)
        c->flags |= PE;
    c->c1 = up;
    for (int i = 0; i < 9; i++) {
        unsigned d = (unsigned)(mag % 100);
        mag /= 100;
        bcd[i] = (uint8_t)(((d / 10) << 4) | (d % 10));
    }
    bcd[9] = a->sign ? 0x80 : 0;
}

X87_INTERNAL x87_f80 x87i_const(int which, uint16_t cw)
{
    /* 66-bit internal constants, rounded according to RC */
    static const struct { uint16_t e; uint64_t hi, lo; } k[6] = {
        { 0x3FFF, 0x8000000000000000ull, 0 },                     /* 1.0 */
        { 0x4000, 0xD49A784BCD1B8AFEull, 0x4000000000000000ull }, /* log2(10) */
        { 0x3FFF, 0xB8AA3B295C17F0BBull, 0xC000000000000000ull }, /* log2(e) */
        { 0x4000, 0xC90FDAA22168C234ull, 0xC000000000000000ull }, /* pi */
        { 0x3FFD, 0x9A209A84FBCFF798ull, 0x8000000000000001ull }, /* log10(2) */
        { 0x3FFE, 0xB17217F7D1CF79ABull, 0xC000000000000000ull }, /* ln(2) */
    };
    fctx c = { cw, 0, 0, 0 };
    if (which < 0 || which > 5)
        return F80(0, 0);
    return x87i_round_pack_prec(&c, 0, k[which].e, k[which].hi, k[which].lo, 64);
}

X87_INTERNAL uint16_t x87i_xam(x87_f80 v)
{
    unp a = x87i_unpack(v);
    switch (a.kind) {
    case K_ZERO: return X87_SW_C3;
    case K_DEN:  return X87_SW_C3 | X87_SW_C2;
    case K_NORM: return X87_SW_C2;
    case K_INF:  return X87_SW_C2 | X87_SW_C0;
    case K_QNAN:
    case K_SNAN: return X87_SW_C0;
    default:     return 0; /* unsupported */
    }
}

/* ------------------------------------------------------------------ */
/* FSCALE, FPREM / FPREM1, FXTRACT                                     */
/* ------------------------------------------------------------------ */

/* An operand passed through unchanged; pseudo-denormals come out normalised. */
static x87_f80 passthru(x87_f80 v)
{
    if (!(v.sexp & 0x7FFF) && (v.mant >> 63))
        v.sexp |= 1;
    return v;
}

X87_INTERNAL x87_f80 x87i_scale(fctx *c, x87_f80 st0, x87_f80 st1)
{
    unp a = x87i_unpack(st0), b = x87i_unpack(st1);
    int32_t n = 0;

    if (a.kind >= K_QNAN || b.kind >= K_QNAN)
        return x87i_nan2(c, &a, &b);
    if (b.kind == K_INF) {
        if (b.sign ? a.kind == K_INF : a.kind == K_ZERO)
            return x87i_invalid(c);
        if (a.kind == K_DEN)
            c->flags |= DE;
        return b.sign ? mk_zero(a.sign) : mk_inf(a.sign);
    }
    if (a.kind == K_DEN || b.kind == K_DEN)
        c->flags |= DE;
    if (a.kind == K_INF || a.kind == K_ZERO)
        return st0;
    if (b.kind == K_ZERO)
        return passthru(st0); /* scaling by zero returns the operand as is */

    {
        int32_t e = b.exp - 16383;
        if (e >= 17)
            n = 0x20000;
        else if (e >= 0)
            n = (int32_t)(b.sig >> (63 - e));
        if (b.sign)
            n = -n;
    }
    /* With the exception unmasked and a result that stays out of range
     * even after the bias adjustment, the hardware delivers a true zero /
     * infinity irrespective of the rounding mode. */
    if (!(c->cw & UE) && a.exp + n + 24576 <= 0) {
        c->flags |= UE | PE;
        return mk_zero(a.sign);
    }
    if (!(c->cw & OE) && a.exp + n - 24576 >= 0x7FFF) {
        c->flags |= OE | PE;
        c->c1 = 1;
        return mk_inf(a.sign);
    }
    /* precision control does not apply */
    return x87i_round_pack_prec(c, a.sign, a.exp + n, a.sig, 0, 64);
}

X87_INTERNAL x87_f80 x87i_prem(fctx *c, x87_f80 st0, x87_f80 st1, int ieee, uint64_t *pq, int *status)
{
    unp a = x87i_unpack(st0), b = x87i_unpack(st1);
    int32_t d, rexp;
    int sign = a.sign, sh;
    uint64_t rem, q = 0;

    *pq = 0;
    *status = 2;
    if (a.kind >= K_QNAN || b.kind >= K_QNAN)
        return x87i_nan2(c, &a, &b);
    if (a.kind == K_INF || b.kind == K_ZERO)
        return x87i_invalid(c);
    *status = 0;
    if (a.kind == K_DEN || b.kind == K_DEN)
        c->flags |= DE;
    if (a.kind == K_ZERO || b.kind == K_INF)
        return passthru(st0);

    d = a.exp - b.exp;
    rexp = b.exp;
    if (d < -1 || (d == -1 && !ieee)) {
        rem = a.sig;
        rexp = a.exp;
    } else if (d == -1) {
        /* FPREM1 with |b|/2 <= |a| < |b|: the quotient rounds to 1 when
         * |a| > |b|/2, giving r = a - b */
        rem = a.sig;
        rexp = a.exp;
        if (a.sig > b.sig) {
            rem = b.sig - (a.sig - b.sig); /* 2b - a, in units of a's ulp */
            q = 1;
            sign ^= 1;
        }
    } else if (d < 64) {
        /* the complete, exact remainder */
        q = div128(d ? a.sig >> (64 - d) : 0, a.sig << d, b.sig, &rem);
        if (ieee && (rem > b.sig - rem || (rem == b.sig - rem && (q & 1)))) {
            rem = b.sig - rem;
            q++;
            sign ^= 1;
        }
    } else {
        /* partial remainder: reduce the exponent difference by 32..63 */
        int n = 32 + (d & 31);
        q = div128(a.sig >> (64 - n), a.sig << n, b.sig, &rem);
        rexp = b.exp + (d - n);
        *status = 1;
    }
    *pq = q;
    if (!rem)
        return mk_zero(a.sign);
    sh = clz64(rem);
    /* exact; rounding only matters for the underflow response */
    return x87i_round_pack_prec(c, sign, rexp - sh, rem << sh, 0, 64);
}

X87_INTERNAL x87_f80 x87i_xtract(fctx *c, x87_f80 st0, x87_f80 *sig)
{
    unp a = x87i_unpack(st0), e;

    if (a.kind >= K_QNAN)
        return *sig = x87i_nan1(c, &a);
    if (a.kind == K_INF) {
        *sig = st0;
        return mk_inf(0);
    }
    if (a.kind == K_ZERO) {
        c->flags |= ZE;
        *sig = st0;
        return mk_inf(1);
    }
    if (a.kind == K_DEN)
        c->flags |= DE;
    *sig = F80((a.sign << 15) | 0x3FFF, a.sig);
    e = x87i_from_int(a.exp - 16383);
    return e.kind == K_ZERO ? mk_zero(0) : F80((e.sign << 15) | e.exp, e.sig);
}
