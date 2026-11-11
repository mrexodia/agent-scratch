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

unp x87i_unpack(x87_f80 v)
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

x87_f80 x87i_pack_raw(const unp *a)
{
    /* only meaningful for classes that keep raw fields */
    return F80((a->sign << 15) | (a->exp & 0x7FFF), a->sig);
}

unp x87i_from_f32(uint32_t v)
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

unp x87i_from_f64(uint64_t v)
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

unp x87i_from_int(int64_t v)
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

x87_f80 x87i_invalid(fctx *c)
{
    /* an invalid operation pre-empts the denormal operand exception */
    c->ex = (uint16_t)((c->ex & ~DE) | IE);
    return F80_DEFAULT_NAN;
}

x87_f80 x87i_nan1(fctx *c, const unp *a)
{
    if (a->kind == K_UNSUP)
        return x87i_invalid(c);
    if (a->kind == K_SNAN)
        c->ex |= IE;
    return F80((a->sign << 15) | 0x7FFF, a->sig | 0x4000000000000000ull);
}

x87_f80 x87i_nan2(fctx *c, const unp *a, const unp *b)
{
    const unp *r;
    int an = a->kind >= K_QNAN, bn = b->kind >= K_QNAN;

    if (a->kind == K_UNSUP || b->kind == K_UNSUP)
        return x87i_invalid(c);
    if (a->kind == K_SNAN || b->kind == K_SNAN)
        c->ex |= IE;
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
                c->ex |= UE;
                if (!adj) {
                    c->abort = 1;
                    return 0;
                }
                exp += adj;
            } else {
                /* masked response (also used when the result is still
                 * out of range after the bias adjustment, as with FSCALE) */
                int shift = 1 - exp;
                if (shift >= 128) {
                    lo = (hi | lo) != 0;
                    hi = 0;
                } else {
                    u128 v = ((u128)hi << 64) | lo;
                    u128 lost = v & (((u128)1 << shift) - 1);
                    v >>= shift;
                    hi = (uint64_t)(v >> 64);
                    lo = (uint64_t)v | (lost != 0);
                }
                up = round_decide(rc, sign, hi, lo, unit, &inexact);
                hi &= ~fmask;
                if (up)
                    hi += unit;
                if (inexact)
                    c->ex |= UE | PE;
                else if (!(c->cw & UE))
                    c->ex |= UE;
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
        c->ex |= OE;
        if (!(c->cw & OE) && (!adj || exp - adj < expinf)) {
            if (!adj) {
                c->abort = 1;
                return 0;
            }
            exp -= adj;
            if (inexact)
                c->ex |= PE;
            c->c1 = up;
        } else {
            int toinf = rc == RC_NEAR || (rc == RC_UP && !sign) || (rc == RC_DOWN && sign);
            c->ex |= PE;
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
            c->ex |= PE;
        c->c1 = up;
    }
    *phi = hi;
    *pexp = exp;
    return 0;
}

x87_f80 x87i_round_pack_prec(fctx *c, int sign, int32_t exp, uint64_t hi, uint64_t lo, int bits)
{
    uint64_t unit = bits == 24 ? 1ull << 40 : bits == 53 ? 1ull << 11 : 1;
    round_core(c, sign, &exp, &hi, lo, unit, 0x7FFF, 24576);
    return F80((sign << 15) | (exp & 0x7FFF), hi);
}

x87_f80 x87i_round_pack(fctx *c, int sign, int32_t exp, uint64_t hi, uint64_t lo)
{
    static const uint8_t bits[4] = { 24, 64, 53, 64 };
    return x87i_round_pack_prec(c, sign, exp, hi, lo, bits[(c->cw >> 8) & 3]);
}

/* ------------------------------------------------------------------ */
/* Basic arithmetic                                                    */
/* ------------------------------------------------------------------ */

static inline x87_f80 mk_inf(int sign) { return F80((sign << 15) | 0x7FFF, 0x8000000000000000ull); }
static inline x87_f80 mk_zero(int sign) { return F80(sign << 15, 0); }

x87_f80 x87i_add(fctx *c, const unp *a, const unp *b, int sub)
{
    int sa = a->sign, sb = b->sign ^ sub;
    int32_t ea, eb, d;
    u128 x, y, s;

    if (a->kind >= K_QNAN || b->kind >= K_QNAN)
        return x87i_nan2(c, a, b);
    if (a->kind == K_INF || b->kind == K_INF) {
        if (a->kind == K_INF && b->kind == K_INF && sa != sb)
            return x87i_invalid(c);
        if (a->kind == K_DEN || b->kind == K_DEN)
            c->ex |= DE;
        return mk_inf(a->kind == K_INF ? sa : sb);
    }
    if (a->kind == K_DEN || b->kind == K_DEN)
        c->ex |= DE;
    if (b->kind == K_ZERO) {
        if (a->kind == K_ZERO)
            return mk_zero(sa == sb ? sa : ctx_rc(c) == RC_DOWN);
        return x87i_round_pack(c, sa, a->exp, a->sig, 0);
    }
    if (a->kind == K_ZERO)
        return x87i_round_pack(c, sb, b->exp, b->sig, 0);

    ea = a->exp; eb = b->exp;
    x = (u128)a->sig << 64;
    y = (u128)b->sig << 64;
    if (ea < eb || (ea == eb && x < y)) {
        u128 t = x; x = y; y = t;
        int32_t te = ea; ea = eb; eb = te;
        int ts = sa; sa = sb; sb = ts;
    }
    d = ea - eb;
    if (d) {
        if (d >= 128) {
            y = 1;
        } else {
            u128 lost = y & (((u128)1 << d) - 1);
            y = (y >> d) | (lost != 0);
        }
    }
    if (sa == sb) {
        s = x + y;
        if (s < x) {
            s = (s >> 1) | (s & 1) | ((u128)1 << 127);
            ea++;
        }
    } else {
        int sh;
        s = x - y;
        if (!s)
            return mk_zero(ctx_rc(c) == RC_DOWN);
        if (s >> 64)
            sh = clz64((uint64_t)(s >> 64));
        else
            sh = 64 + clz64((uint64_t)s);
        s <<= sh;
        ea -= sh;
    }
    return x87i_round_pack(c, sa, ea, (uint64_t)(s >> 64), (uint64_t)s);
}

x87_f80 x87i_mul(fctx *c, const unp *a, const unp *b)
{
    int sign = a->sign ^ b->sign;
    int32_t exp;
    u128 p;

    if (a->kind >= K_QNAN || b->kind >= K_QNAN)
        return x87i_nan2(c, a, b);
    if (a->kind == K_INF || b->kind == K_INF) {
        if (a->kind == K_ZERO || b->kind == K_ZERO)
            return x87i_invalid(c);
        if (a->kind == K_DEN || b->kind == K_DEN)
            c->ex |= DE;
        return mk_inf(sign);
    }
    if (a->kind == K_DEN || b->kind == K_DEN)
        c->ex |= DE;
    if (a->kind == K_ZERO || b->kind == K_ZERO)
        return mk_zero(sign);

    p = (u128)a->sig * b->sig;
    exp = a->exp + b->exp - 16383 + 1;
    if (!(p >> 127)) {
        p <<= 1;
        exp--;
    }
    return x87i_round_pack(c, sign, exp, (uint64_t)(p >> 64), (uint64_t)p);
}

x87_f80 x87i_div(fctx *c, const unp *a, const unp *b)
{
    int sign = a->sign ^ b->sign;
    int32_t exp;
    u128 n, r;
    uint64_t q, q2;

    if (a->kind >= K_QNAN || b->kind >= K_QNAN)
        return x87i_nan2(c, a, b);
    if (a->kind == K_INF) {
        if (b->kind == K_INF)
            return x87i_invalid(c);
        if (b->kind == K_DEN)
            c->ex |= DE;
        return mk_inf(sign);
    }
    if (b->kind == K_INF) {
        if (a->kind == K_DEN)
            c->ex |= DE;
        return mk_zero(sign);
    }
    if (b->kind == K_ZERO) {
        if (a->kind == K_ZERO)
            return x87i_invalid(c);
        c->ex |= ZE; /* no #D for a denormal dividend here */
        return mk_inf(sign);
    }
    if (a->kind == K_DEN || b->kind == K_DEN)
        c->ex |= DE;
    if (a->kind == K_ZERO)
        return mk_zero(sign);

    exp = a->exp - b->exp + 16383;
    if (a->sig >= b->sig) {
        n = (u128)a->sig << 63;
    } else {
        n = (u128)a->sig << 64;
        exp--;
    }
    q = (uint64_t)(n / b->sig);
    r = n % b->sig;
    r <<= 64;
    q2 = (uint64_t)(r / b->sig);
    if (r % b->sig)
        q2 |= 1;
    return x87i_round_pack(c, sign, exp, q, q2);
}

x87_f80 x87i_sqrt(fctx *c, const unp *a)
{
    u128 n, rem, s, bit;
    int32_t e;
    uint64_t lo;

    if (a->kind >= K_QNAN)
        return x87i_nan1(c, a);
    if (a->kind == K_ZERO)
        return mk_zero(a->sign);
    if (a->sign)
        return x87i_invalid(c);
    if (a->kind == K_INF)
        return mk_inf(0);
    if (a->kind == K_DEN)
        c->ex |= DE;

    /* value = sig * 2^(e-63) with e unbiased; make the exponent even */
    e = a->exp - 16383;
    if (e & 1) {
        n = (u128)a->sig << 63; /* sig * 2^63, exponent e+1... handled below */
        n <<= 1;                /* sig * 2^64 */
        e -= 1;
    } else {
        n = (u128)a->sig << 63;
    }
    /* now value = n * 2^(e - 126), n in [2^126, 2^128); sqrt = isqrt(n) * 2^(e/2 - 63) */
    s = 0; rem = 0;
    /* digit-by-digit integer square root of a 128-bit value */
    bit = (u128)1 << 126;
    rem = n;
    while (bit) {
        u128 t = s + bit;
        if (rem >= t) {
            rem -= t;
            s = (s >> 1) + bit;
        } else {
            s >>= 1;
        }
        bit >>= 2;
    }
    /* s = floor(sqrt(n)) (64 bits, top bit set), rem = n - s^2 */
    lo = rem > s ? 0xC000000000000000ull : rem ? 0x4000000000000000ull : 0;
    return x87i_round_pack(c, 0, (e >> 1) + 16383, (uint64_t)s, lo);
}

/* ------------------------------------------------------------------ */
/* Integer rounding and conversions                                    */
/* ------------------------------------------------------------------ */

int x87i_round_mag(int rc, const unp *a, uint64_t *mag, int *inexact, int *roundup)
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

int64_t x87i_to_int(fctx *c, const unp *a, int bits, int trunc)
{
    uint64_t mag, lim = 1ull << (bits - 1);
    int64_t indef = (int64_t)(0 - lim);
    int inexact, up;

    if (a->kind >= K_INF) {
        c->ex |= IE;
        return indef;
    }
    if (x87i_round_mag(trunc ? RC_ZERO : ctx_rc(c), a, &mag, &inexact, &up) ||
        mag > lim || (mag == lim && !a->sign)) {
        c->ex |= IE;
        return indef;
    }
    if (inexact)
        c->ex |= PE;
    c->c1 = up;
    return a->sign ? (int64_t)(0 - mag) : (int64_t)mag;
}

x87_f80 x87i_rndint(fctx *c, const unp *a)
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
        c->ex |= DE;
    if (a->exp >= 16383 + 63)
        return F80((a->sign << 15) | a->exp, a->sig);
    x87i_round_mag(ctx_rc(c), a, &mag, &inexact, &up);
    if (inexact)
        c->ex |= PE;
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
        c->ex |= IE;
        return ((uint64_t)1 << (fbits + (expinf == 0xFF ? 8 : 11))) | einf | (1ull << (fbits - 1));
    case K_SNAN:
        c->ex |= IE;
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
    if (c->abort)
        return 0;
    return sign | ((uint64_t)exp << fbits) | ((hi << 1) >> (64 - fbits));
}

uint32_t x87i_to_f32(fctx *c, x87_f80 v)
{
    return (uint32_t)to_float(c, v, 1ull << 40, 23, 127, 0xFF);
}

uint64_t x87i_to_f64(fctx *c, x87_f80 v)
{
    return to_float(c, v, 1ull << 11, 52, 1023, 0x7FF);
}

/* ------------------------------------------------------------------ */
/* Comparison                                                          */
/* ------------------------------------------------------------------ */

int x87i_cmp(fctx *c, const unp *a, const unp *b, int quiet)
{
    int r;
    if (a->kind >= K_QNAN || b->kind >= K_QNAN) {
        if (!quiet || a->kind >= K_SNAN || b->kind >= K_SNAN)
            c->ex |= IE;
        return 3;
    }
    if (a->kind == K_DEN || b->kind == K_DEN)
        c->ex |= DE;
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
