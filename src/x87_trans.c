/* SPDX-License-Identifier: MIT
 *
 * Transcendental instructions.
 *
 * Values are computed with a small 192-bit software floating point type and
 * then rounded once, so the result is the correctly rounded value of the
 * mathematical function - with the hardware's documented deviations:
 * trigonometric argument reduction uses Intel's 66-bit approximation of pi,
 * and a number of special / out-of-range cases behave as on real CPUs.
 */
#include "x87_internal.h"

/* ------------------------------------------------------------------ */
/* 192-bit floating point                                              */
/* ------------------------------------------------------------------ */

typedef struct bf {
    int      sign;
    int32_t  exp;  /* value in [2^exp, 2^(exp+1)) */
    uint64_t m[3]; /* m[2] most significant, top bit set; all zero = 0 */
} bf;

static int bf_is_zero(const bf *a) { return !a->m[2]; }

static void bf_normalize(bf *a)
{
    int s;
    while (!a->m[2]) {
        if (!(a->m[1] | a->m[0])) {
            a->sign = 0;
            a->exp = 0;
            return;
        }
        a->m[2] = a->m[1];
        a->m[1] = a->m[0];
        a->m[0] = 0;
        a->exp -= 64;
    }
    s = clz64(a->m[2]);
    if (s) {
        a->m[2] = (a->m[2] << s) | (a->m[1] >> (64 - s));
        a->m[1] = (a->m[1] << s) | (a->m[0] >> (64 - s));
        a->m[0] <<= s;
        a->exp -= s;
    }
}

/* (hi:lo) * 2^e2 */
static bf bf_from_w(uint64_t hi, uint64_t lo, int32_t e2, int sign)
{
    bf r;
    r.sign = sign;
    r.m[2] = 0;
    r.m[1] = hi;
    r.m[0] = lo;
    r.exp = e2 + 191;
    bf_normalize(&r);
    return r;
}

static bf bf_from_unp(const unp *a)
{
    bf r;
    r.sign = a->sign;
    r.exp = a->exp - 16383;
    r.m[2] = a->sig;
    r.m[1] = r.m[0] = 0;
    return r;
}

static void shr192(uint64_t *m, int n)
{
    while (n >= 64) {
        m[0] = m[1]; m[1] = m[2]; m[2] = 0;
        n -= 64;
    }
    if (n) {
        m[0] = (m[0] >> n) | (m[1] << (64 - n));
        m[1] = (m[1] >> n) | (m[2] << (64 - n));
        m[2] >>= n;
    }
}

static int cmp192(const uint64_t *a, const uint64_t *b)
{
    for (int i = 2; i >= 0; i--)
        if (a[i] != b[i])
            return a[i] > b[i] ? 1 : -1;
    return 0;
}

static bf bf_add(const bf *a, const bf *b)
{
    bf x, y;
    int32_t d;
    if (bf_is_zero(a)) return *b;
    if (bf_is_zero(b)) return *a;
    if (a->exp > b->exp || (a->exp == b->exp && cmp192(a->m, b->m) >= 0)) {
        x = *a; y = *b;
    } else {
        x = *b; y = *a;
    }
    d = x.exp - y.exp;
    if (d >= 192)
        return x;
    shr192(y.m, d);
    if (x.sign == y.sign) {
        unsigned carry = 0;
        for (int i = 0; i < 3; i++) {
            uint64_t t = x.m[i] + y.m[i], u = t + carry;
            carry = (t < x.m[i]) | (u < t);
            x.m[i] = u;
        }
        if (carry) {
            shr192(x.m, 1);
            x.m[2] |= 0x8000000000000000ull;
            x.exp++;
        }
    } else {
        unsigned borrow = 0;
        for (int i = 0; i < 3; i++) {
            uint64_t t = x.m[i] - y.m[i], u = t - borrow;
            borrow = (x.m[i] < y.m[i]) | (t < borrow);
            x.m[i] = u;
        }
        bf_normalize(&x);
    }
    return x;
}

static bf bf_neg(bf a)
{
    if (!bf_is_zero(&a))
        a.sign ^= 1;
    return a;
}

static bf bf_sub(const bf *a, const bf *b)
{
    bf nb = bf_neg(*b);
    return bf_add(a, &nb);
}

static bf bf_mul(const bf *a, const bf *b)
{
    uint64_t p[6] = { 0, 0, 0, 0, 0, 0 };
    bf r;
    if (bf_is_zero(a) || bf_is_zero(b)) {
        r.sign = 0; r.exp = 0; r.m[0] = r.m[1] = r.m[2] = 0;
        return r;
    }
    for (int i = 0; i < 3; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 3; j++) {
            uint64_t hi, lo = mul64(a->m[i], b->m[j], &hi);
            lo += p[i + j];
            hi += lo < p[i + j];
            lo += carry;
            hi += lo < carry;
            p[i + j] = lo;
            carry = hi;
        }
        p[i + 3] = carry;
    }
    r.sign = a->sign ^ b->sign;
    if (p[5] >> 63) {
        r.m[2] = p[5]; r.m[1] = p[4]; r.m[0] = p[3];
        r.exp = a->exp + b->exp + 1;
    } else {
        r.m[2] = (p[5] << 1) | (p[4] >> 63);
        r.m[1] = (p[4] << 1) | (p[3] >> 63);
        r.m[0] = (p[3] << 1) | (p[2] >> 63);
        r.exp = a->exp + b->exp;
    }
    return r;
}

static const bf BF_ONE = { 0, 0, { 0, 0, 0x8000000000000000ull } };
static const bf BF_TWO = { 0, 1, { 0, 0, 0x8000000000000000ull } };
static const bf BF_PI = { 0, 1, { 0x29024E088A67CC74ull, 0xC4C6628B80DC1CD1ull, 0xC90FDAA22168C234ull } };
static const bf BF_LN2 = { 0, -1, { 0x40F343267298B62Dull, 0xC9E3B39803F2F6AFull, 0xB17217F7D1CF79ABull } };
static const bf BF_INVLN2 = { 0, 0, { 0xEB577AA8DD695A58ull, 0xBE87FED0691D3E88ull, 0xB8AA3B295C17F0BBull } };

static bf bf_div(const bf *a, const bf *b)
{
    bf x, t;
    uint64_t rem;
    if (b->m[2] == 0x8000000000000000ull && !(b->m[1] | b->m[0])) {
        /* power of two: exact */
        x = *a;
        if (!bf_is_zero(&x)) {
            x.exp -= b->exp;
            x.sign ^= b->sign;
        }
        return x;
    }
    /* initial 64-bit reciprocal estimate: 2^127 / top limb */
    x.sign = b->sign;
    x.exp = -b->exp - 1;
    x.m[2] = b->m[2] == 0x8000000000000000ull ? ~0ull : div128(0x8000000000000000ull, 0, b->m[2], &rem);
    x.m[1] = x.m[0] = 0;
    /* the estimate is good to ~62 bits; each step doubles that */
    for (int i = 0; i < 2; i++) {
        t = bf_mul(b, &x);
        t = bf_sub(&BF_TWO, &t);
        x = bf_mul(&x, &t);
    }
    return bf_mul(a, &x);
}

static bf bf_scale(bf a, int n)
{
    if (!bf_is_zero(&a))
        a.exp += n;
    return a;
}

/* Truncate to the 67 significant bits the hardware carries internally. */
static bf bf_chop67(bf a)
{
    a.m[1] &= 0xE000000000000000ull;
    a.m[0] = 0;
    return a;
}

static x87_f80 bf_round(fctx *c, const bf *a)
{
    if (bf_is_zero(a))
        return F80(0, 0);
    return x87i_round_pack_prec(c, a->sign, a->exp + 16383, a->m[2], a->m[1] | (a->m[0] != 0), 64);
}

/* Round a transcendental result.  The hardware always reports these as
 * inexact, and a (masked) denormal result therefore also underflows. */
static x87_f80 bf_finish(fctx *c, const bf *a)
{
    x87_f80 r = bf_round(c, a);
    c->flags |= PE;
    if (!(r.sexp & 0x7FFF) && (c->cw & UE))
        c->flags |= UE;
    return r;
}

/* ------------------------------------------------------------------ */
/* Kernels                                                             */
/* ------------------------------------------------------------------ */

#include "x87_tables.h"

#define BF_TERM_BITS 200

/*
 * Power series, evaluated in 192-bit fixed point.
 *
 * All series here have the form P(x) = sum c_k x^k with c_0 = 1, 0 < x < 1
 * and terms of decreasing magnitude, so every intermediate value of
 * Horner's rule lies in (0, 2) and plain Q2.190 integers suffice - no
 * alignment or normalisation per term.
 */

/* Number of terms needed so that c_n * x^n is below 2^-BF_TERM_BITS, from
 * an upper bound on log2(x) in 1/128ths. */
static int series_len(const bf *x, const int16_t *tab_exp, int max)
{
    /* log2(1 + f) <= f + 0.09 */
    int32_t lg128 = x->exp * 128 + (int32_t)(x->m[2] >> 56) - 127 + 12;
    int n = 1;
    if (lg128 > -1)
        lg128 = -1;
    while (n < max && tab_exp[n] * 128 + n * lg128 > -BF_TERM_BITS * 128)
        n++;
    return n;
}

/* high 192 bits of a (Q2.190) times x (Q0.192); the partial products that
 * only reach the discarded low half are skipped (error of a few 2^-190) */
static void fx_mulhi(uint64_t r[3], const uint64_t a[3], const uint64_t x[3])
{
    uint64_t h, l, c0, c1, c2, c3 = 0, t;
    /* weight 2^128: high halves of the i + j == 2 products */
    (void)mul64(a[0], x[2], &c0);
    (void)mul64(a[1], x[1], &h); c0 += h; c1 = c0 < h;
    (void)mul64(a[2], x[0], &h); c0 += h; c1 += c0 < h;
    /* i + j == 3 */
    l = mul64(a[1], x[2], &h); c0 += l; t = c0 < l; c1 += h; c2 = c1 < h; c1 += t; c2 += c1 < t;
    l = mul64(a[2], x[1], &h); c0 += l; t = c0 < l; c1 += h; c2 += c1 < h; c1 += t; c2 += c1 < t;
    /* i + j == 4 */
    l = mul64(a[2], x[2], &h); c1 += l; t = c1 < l; c2 += h; c3 += c2 < h; c2 += t;
    (void)c3;
    r[0] = c0; r[1] = c1; r[2] = c2;
}

/* sum_{k=0..n} (alt ? (-1)^k : 1) * c_k * x^k for 0 < x < 1 */
static bf series(const bf *x, const uint64_t tab[][3], const int16_t *tab_exp, int max, int alt)
{
    int n = series_len(x, tab_exp, max);
    uint64_t fx[3] = { x->m[0], x->m[1], x->m[2] }, acc[3], t[3];
    bf r;

    shr192(fx, -1 - x->exp); /* x as a Q0.192 fraction */
    acc[0] = tab[n][0]; acc[1] = tab[n][1]; acc[2] = tab[n][2];
    for (int k = n - 1; k >= 0; k--) {
        unsigned carry = 0;
        fx_mulhi(t, acc, fx);
        if (alt) {
            /* c_k - acc * x */
            for (int i = 0; i < 3; i++) {
                uint64_t d = tab[k][i] - t[i], u = d - carry;
                carry = (tab[k][i] < t[i]) | (d < carry);
                acc[i] = u;
            }
        } else {
            for (int i = 0; i < 3; i++) {
                uint64_t d = tab[k][i] + t[i], u = d + carry;
                carry = (d < t[i]) | (u < d);
                acc[i] = u;
            }
        }
    }
    r.sign = 0;
    r.exp = 1; /* Q2.190 */
    r.m[0] = acc[0]; r.m[1] = acc[1]; r.m[2] = acc[2];
    bf_normalize(&r);
    return r;
}

#define SERIES(x, tab, max, alt) series(x, tab, tab##_EXP, max, alt)

static bf k_sin(const bf *r)
{
    bf r2 = bf_mul(r, r), p = SERIES(&r2, TAB_INVFACT_ODD, 26, 1);
    return bf_mul(r, &p);
}

static bf k_cos(const bf *r)
{
    bf r2 = bf_mul(r, r);
    return SERIES(&r2, TAB_INVFACT_EVEN, 26, 1);
}

/* atan for 0 <= z <= 1 */
static bf k_atan(const bf *z)
{
    static const bf thr = { 0, -2, { 0, 0, 0xD413CCCFE7799211ull } }; /* tan(pi/8) */
    bf w = *z, sum;
    int reduce = 0;
    if (bf_is_zero(z))
        return *z;
    if (z->exp > thr.exp || (z->exp == thr.exp && cmp192(z->m, thr.m) > 0)) {
        /* atan(z) = pi/4 - atan((1 - z) / (1 + z)) */
        bf n = bf_sub(&BF_ONE, z), d = bf_add(&BF_ONE, z);
        w = bf_div(&n, &d);
        reduce = 1;
    }
    /* second reduction: atan(w) = atan(c) + atan((w - c) / (1 + w c)) with
     * c = k/8 nearest to w, leaving an argument below 1/16 */
    {
        static const bf atan_k8[3] = {
    { 0, -4, { 0x8B3957D95D9AD922ull, 0xC897989F3E888EF7ull, 0xFEADD4D5617B6E32ull } }, /* atan(1/8) */
    { 0, -3, { 0xAA7FA90388B3836Bull, 0x6DC79EF5F7A217E5ull, 0xFADBAFC96406EB15ull } }, /* atan(2/8) */
    { 0, -2, { 0x3D843977AE5455CCull, 0x8AA32122DCFE4483ull, 0xB7B0CA0F26F78473ull } }, /* atan(3/8) */
        };
        int k = 0;
        if (w.exp >= -4) {
            /* round(8 w) for w < 0.4143 */
            uint64_t w8 = w.m[2] >> (60 - w.exp - 1); /* w * 16, truncated */
            k = (int)((w8 + 1) >> 1);
        }
        if (k) {
            bf c = bf_from_w(0, (uint64_t)k, -3, 0);
            bf n = bf_sub(&w, &c), d = bf_mul(&w, &c);
            d = bf_add(&BF_ONE, &d);
            w = bf_div(&n, &d);
        }
        sum = w;
        if (!bf_is_zero(&w)) {
            bf w2 = bf_mul(&w, &w), p = SERIES(&w2, TAB_ODD_RECIP, 83, 1);
            sum = bf_mul(&w, &p);
        }
        if (k)
            sum = bf_add(&atan_k8[k - 1], &sum);
    }
    if (reduce) {
        bf pi4 = bf_scale(BF_PI, -2);
        sum = bf_sub(&pi4, &sum);
    }
    return sum;
}

/* 2*atanh(t) = ln((1+t)/(1-t)) */
static bf k_atanh2(const bf *t)
{
    bf t2, p;
    if (bf_is_zero(t))
        return *t;
    t2 = bf_mul(t, t);
    p = SERIES(&t2, TAB_ODD_RECIP, 83, 0);
    p = bf_mul(t, &p);
    return bf_scale(p, 1);
}

/* log2 of a positive value */
static bf k_log2(const bf *x)
{
    bf f = *x, n, d, t, l, e;
    int32_t ex = x->exp;
    f.exp = 0;
    if (f.m[2] > 0xB504F333F9DE6484ull) {
        f.exp = -1;
        ex++;
    }
    n = bf_sub(&f, &BF_ONE);
    d = bf_add(&f, &BF_ONE);
    t = bf_div(&n, &d);
    l = k_atanh2(&t);
    l = bf_mul(&l, &BF_INVLN2);
    e = bf_from_w(0, ex < 0 ? (uint64_t)(-(int64_t)ex) : (uint64_t)ex, 0, ex < 0);
    return bf_add(&e, &l);
}

static bf k_expm1(const bf *z)
{
    bf az = *z, p;
    if (bf_is_zero(z))
        return *z;
    /* (e^z - 1) / z = sum z^k / (k+1)!, alternating for negative z */
    az.sign = 0;
    p = SERIES(&az, TAB_INVFACT, 49, z->sign);
    return bf_mul(z, &p);
}

/* ------------------------------------------------------------------ */
/* Trigonometric functions                                             */
/* ------------------------------------------------------------------ */

/* Intel's 66-bit pi: 0xC90FDAA22168C234C * 2^-66 */
#define PI66_HI 0xCull
#define PI66_LO 0x90FDAA22168C234Cull

/* Reduce |x| modulo pi66/2 to [-pi/4, pi/4]; returns the quadrant. */
static unsigned trig_reduce(const unp *a, bf *r)
{
    int32_t e = a->exp - 16383;
    w128 rem, p = w_make(PI66_HI, PI66_LO);
    uint64_t q = 0;
    int neg = 0;

    if (e < -1) {
        *r = bf_from_unp(a);
        r->sign = 0;
        return 0;
    }
    /* Exact long division in units of 2^-67, in which pi66/2 == PI66:
     * |x| = sig << (e + 4).  Only the low bits of the quotient matter. */
    rem = w_make(0, a->sig);
    for (int i = e + 4; i > 0; i--) {
        rem = w_shl(rem, 1);
        q <<= 1;
        if (w_cmp(rem, p) >= 0) {
            rem = w_sub(rem, p);
            q |= 1;
        }
    }
    if (w_cmp(rem, w_shr(p, 1)) > 0) {
        rem = w_sub(p, rem);
        q++;
        neg = 1;
    }
    *r = bf_from_w(rem.hi, rem.lo, -67, neg);
    return (unsigned)(q & 3);
}

/* which: 0 sin, 1 cos.  Handles finite non-zero |x| < 2^63. */
static bf trig_eval(const unp *a, int which)
{
    bf r, v;
    unsigned n = trig_reduce(a, &r);
    if (which)
        n++;
    v = (n & 1) ? k_cos(&r) : k_sin(&r);
    if (n & 2)
        v.sign ^= 1;
    if (!which && a->sign)
        v.sign ^= 1;
    return v;
}

/* Common operand screening; returns 1 if *out holds the final result. */
static int trig_special(fctx *c, const unp *a, int *c2, x87_f80 *out)
{
    *c2 = 0;
    *out = F80(0, 0);
    if (a->kind >= K_QNAN) {
        *out = x87i_nan1(c, a);
        return 1;
    }
    if (a->kind == K_INF) {
        *out = x87i_invalid(c);
        return 1;
    }
    if (a->kind == K_DEN)
        c->flags |= DE;
    if (a->kind != K_ZERO && a->exp >= 16383 + 63) {
        *c2 = 1;
        return 1;
    }
    return 0;
}

/* For very small arguments the hardware returns the argument itself
 * (flagged inexact, never "rounded up"). */
static x87_f80 tiny_identity(fctx *c, const unp *a)
{
    x87_f80 r = x87i_round_pack_prec(c, a->sign, a->exp, a->sig, 0, 64);
    c->flags |= PE;
    if (a->exp <= 0 && (c->cw & UE))
        c->flags |= UE;
    return r;
}

#define SIN_TINY_EXP (16383 - 69)
#define TAN_TINY_EXP (16383 - 34)
#define ATAN_TINY_EXP (-41)

X87_INTERNAL x87_f80 x87i_fsin(fctx *c, const unp *a, int *c2)
{
    x87_f80 r;
    bf v;
    if (trig_special(c, a, c2, &r))
        return r;
    if (a->kind == K_ZERO)
        return F80(a->sign << 15, 0);
    if (a->exp <= SIN_TINY_EXP)
        return tiny_identity(c, a);
    v = trig_eval(a, 0);
    r = bf_finish(c, &v);
    return r;
}

X87_INTERNAL x87_f80 x87i_fcos(fctx *c, const unp *a, int *c2)
{
    x87_f80 r;
    bf v;
    if (trig_special(c, a, c2, &r))
        return r;
    if (a->kind == K_ZERO)
        return F80(0x3FFF, 0x8000000000000000ull);
    if (a->exp <= SIN_TINY_EXP) {
        c->flags |= PE;
        return F80(0x3FFF, 0x8000000000000000ull);
    }
    v = trig_eval(a, 1);
    r = bf_finish(c, &v);
    return r;
}

X87_INTERNAL void x87i_fsincos(fctx *c, const unp *a, x87_f80 *s, x87_f80 *co, int *c2)
{
    bf v;
    if (trig_special(c, a, c2, s)) {
        *co = *s;
        return;
    }
    if (a->kind == K_ZERO) {
        *s = F80(a->sign << 15, 0);
        *co = F80(0x3FFF, 0x8000000000000000ull);
        return;
    }
    if (a->exp <= SIN_TINY_EXP) {
        *s = tiny_identity(c, a);
        *co = F80(0x3FFF, 0x8000000000000000ull);
        return;
    }
    v = trig_eval(a, 0);
    *s = bf_round(c, &v);
    v = trig_eval(a, 1);
    *co = bf_round(c, &v);
    c->flags |= PE;
}

X87_INTERNAL x87_f80 x87i_fptan(fctx *c, const unp *a, int *c2)
{
    x87_f80 r;
    bf rr, s, co, v;
    unsigned n;
    if (trig_special(c, a, c2, &r))
        return r;
    if (a->kind == K_ZERO)
        return F80(a->sign << 15, 0);
    if (a->exp <= SIN_TINY_EXP)
        return tiny_identity(c, a);
    if (a->exp <= TAN_TINY_EXP) {
        /* Between 2^-69 and 2^-33 the hardware's tangent is its sine
         * (x minus a little) over a cosine of exactly 1: the result
         * rounds as a value just below |x|, although tan(x) > x. */
        bf eps;
        v = bf_from_unp(a);
        eps = bf_from_w(0, 1, v.exp - 160, v.sign);
        v = bf_sub(&v, &eps);
        return bf_finish(c, &v);
    }
    n = trig_reduce(a, &rr);
    s = k_sin(&rr);
    co = k_cos(&rr);
    if (n & 1) {
        v = bf_div(&co, &s);
        v.sign ^= 1;
    } else {
        v = bf_div(&s, &co);
    }
    if (a->sign)
        v.sign ^= 1;
    r = bf_finish(c, &v);
    return r;
}

/* ------------------------------------------------------------------ */
/* FPATAN                                                              */
/* ------------------------------------------------------------------ */

static x87_f80 round_signed(fctx *c, bf v, int sign)
{
    x87_f80 r;
    v.sign = sign;
    r = bf_finish(c, &v);
    return r;
}

X87_INTERNAL x87_f80 x87i_fpatan(fctx *c, const unp *y, const unp *x)
{
    bf by, bx, z, t;
    int swap;

    if (x->kind >= K_QNAN || y->kind >= K_QNAN)
        return x87i_nan2(c, x, y);
    if (x->kind == K_DEN || y->kind == K_DEN)
        c->flags |= DE;
    if (y->kind == K_ZERO) {
        if (!x->sign)
            return F80(y->sign << 15, 0);
        return round_signed(c, BF_PI, y->sign);
    }
    if (x->kind == K_ZERO)
        return round_signed(c, bf_scale(BF_PI, -1), y->sign);
    if (y->kind == K_INF) {
        if (x->kind == K_INF) {
            bf q = bf_scale(BF_PI, -2);
            if (x->sign) {
                bf h = bf_scale(BF_PI, -1);
                q = bf_add(&q, &h);
            }
            return round_signed(c, q, y->sign);
        }
        return round_signed(c, bf_scale(BF_PI, -1), y->sign);
    }
    if (x->kind == K_INF) {
        if (!x->sign)
            return F80(y->sign << 15, 0);
        return round_signed(c, BF_PI, y->sign);
    }
    if (!x->sign && y->exp - x->exp <= ATAN_TINY_EXP) {
        /* For a tiny ratio and positive x the hardware returns the
         * quotient y / x itself (rounded to 64 bits, flagged inexact). */
        fctx q = { (uint16_t)(c->cw | 0x0300), 0, 0, 0 };
        x87_f80 r = x87i_div(&q, y, x);
        c->flags |= (q.flags & (UE | OE)) | PE;
        c->c1 = q.c1;
        if (!(r.sexp & 0x7FFF) && (c->cw & UE))
            c->flags |= UE;
        return r;
    }
    by = bf_from_unp(y); by.sign = 0;
    bx = bf_from_unp(x); bx.sign = 0;
    swap = by.exp > bx.exp || (by.exp == bx.exp && by.m[2] > bx.m[2]);
    z = swap ? bf_div(&bx, &by) : bf_div(&by, &bx);
    t = k_atan(&z);
    if (swap) {
        bf h = bf_scale(BF_PI, -1);
        t = bf_sub(&h, &t);
    }
    if (x->sign)
        t = bf_sub(&BF_PI, &t);
    return round_signed(c, t, y->sign);
}

/* ------------------------------------------------------------------ */
/* F2XM1, FYL2X, FYL2XP1                                               */
/* ------------------------------------------------------------------ */

X87_INTERNAL x87_f80 x87i_f2xm1(fctx *c, const unp *a)
{
    bf x, z, v;
    x87_f80 r;
    if (a->kind >= K_QNAN)
        return x87i_nan1(c, a);
    if (a->kind == K_ZERO)
        return F80(a->sign << 15, 0);
    if (a->kind == K_INF)
        return a->sign ? F80(0xBFFF, 0x8000000000000000ull) : F80(0x7FFF, 0x8000000000000000ull);
    if (a->kind == K_DEN)
        c->flags |= DE;
    if (a->exp >= 16383) {
        /* |x| >= 1 */
        c->flags |= PE;
        if (a->exp == 16383 && a->sig == 0x8000000000000000ull)
            return a->sign ? F80(0xBFFE, 0x8000000000000000ull) : F80(0x3FFF, 0x8000000000000000ull);
        return F80((a->sign << 15) | a->exp, a->sig); /* out of range: unchanged */
    }
    /* (2^x - 1) / x truncated to 67 bits, times x: the best fit to what
     * the hardware produces */
    x = bf_from_unp(a);
    z = bf_mul(&x, &BF_LN2);
    v = k_expm1(&z);
    v = bf_div(&v, &x);
    v = bf_chop67(v);
    v = bf_mul(&v, &x);
    r = bf_finish(c, &v);
    return r;
}

static x87_f80 inf_signed(int sign) { return F80((sign << 15) | 0x7FFF, 0x8000000000000000ull); }

X87_INTERNAL x87_f80 x87i_fyl2x(fctx *c, const unp *y, const unp *x)
{
    bf l, by, v;
    x87_f80 r;

    if (x->kind >= K_QNAN || y->kind >= K_QNAN)
        return x87i_nan2(c, x, y);
    if (x->kind == K_ZERO) {
        if (y->kind == K_ZERO)
            return x87i_invalid(c);
        if (y->kind != K_INF)
            c->flags |= ZE;
        return inf_signed(!y->sign);
    }
    if (x->sign)
        return x87i_invalid(c);
    if (x->kind == K_DEN || y->kind == K_DEN)
        c->flags |= DE;
    if (x->kind == K_INF) {
        if (y->kind == K_ZERO)
            return x87i_invalid(c);
        return inf_signed(y->sign);
    }
    if (x->exp == 16383 && x->sig == 0x8000000000000000ull) {
        if (y->kind == K_INF)
            return x87i_invalid(c);
        return F80(y->sign << 15, 0);
    }
    if (y->kind == K_INF)
        return inf_signed(y->sign ^ (x->exp < 16383));
    if (y->kind == K_ZERO)
        return F80((y->sign ^ (x->exp < 16383)) << 15, 0);
    by = bf_from_unp(x);
    l = k_log2(&by);
    if (x->sig == 0x8000000000000000ull && x->exp < 16383) {
        /* For x = 2^-n the hardware's logarithm comes out a hair above
         * the exact integer -n. */
        bf eps = bf_from_w(0, 1, l.exp - 160, 0);
        l = bf_add(&l, &eps);
    }
    /* Measured against real hardware: the logarithm is truncated to 67
     * bits before the multiplication by y. */
    l = bf_chop67(l);
    by = bf_from_unp(y);
    v = bf_mul(&l, &by);
    r = bf_finish(c, &v);
    return r;
}

X87_INTERNAL x87_f80 x87i_fyl2xp1(fctx *c, const unp *y, const unp *x)
{
    bf bx, l, by, v;
    x87_f80 r;

    if (x->kind >= K_QNAN || y->kind >= K_QNAN)
        return x87i_nan2(c, x, y);
    if (x->kind == K_DEN || y->kind == K_DEN)
        c->flags |= DE;
    if (x->kind == K_ZERO) {
        if (y->kind == K_INF)
            return x87i_invalid(c);
        return F80((x->sign ^ y->sign) << 15, 0);
    }
    if (x->kind == K_INF) {
        if (x->sign || y->kind == K_ZERO)
            return x87i_invalid(c);
        return inf_signed(y->sign);
    }
    if (y->kind == K_INF)
        return inf_signed(x->sign ^ y->sign);
    if (y->kind == K_ZERO)
        return F80((x->sign ^ y->sign) << 15, 0);
    bx = bf_from_unp(x);
    if (x->exp < 16383 - 2) {
        /* ln(1+x) = 2 atanh(x / (2 + x)) */
        bf d = bf_add(&BF_TWO, &bx), t = bf_div(&bx, &d);
        l = k_atanh2(&t);
        l = bf_mul(&l, &BF_INVLN2);
    } else {
        bf s = bf_add(&BF_ONE, &bx);
        if (bf_is_zero(&s) || s.sign) {
            /* x <= -1: outside the domain; the hardware returns x */
            c->flags |= PE;
            return F80((x->sign << 15) | x->exp, x->sig);
        }
        l = k_log2(&s);
    }
    l = bf_chop67(l);
    by = bf_from_unp(y);
    v = bf_mul(&l, &by);
    r = bf_finish(c, &v);
    return r;
}
