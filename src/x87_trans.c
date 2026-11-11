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

/* v * 2^e2 */
static bf bf_from_u128(u128 v, int32_t e2, int sign)
{
    bf r;
    r.sign = sign;
    r.m[2] = 0;
    r.m[1] = (uint64_t)(v >> 64);
    r.m[0] = (uint64_t)v;
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
        u128 t = (u128)x.m[0] + y.m[0];
        x.m[0] = (uint64_t)t;
        t = (u128)x.m[1] + y.m[1] + (uint64_t)(t >> 64);
        x.m[1] = (uint64_t)t;
        t = (u128)x.m[2] + y.m[2] + (uint64_t)(t >> 64);
        x.m[2] = (uint64_t)t;
        if (t >> 64) {
            shr192(x.m, 1);
            x.m[2] |= 0x8000000000000000ull;
            x.exp++;
        }
    } else {
        u128 t = (u128)x.m[0] - y.m[0];
        x.m[0] = (uint64_t)t;
        t = (u128)x.m[1] - y.m[1] - (uint64_t)((t >> 64) & 1);
        x.m[1] = (uint64_t)t;
        t = (u128)x.m[2] - y.m[2] - (uint64_t)((t >> 64) & 1);
        x.m[2] = (uint64_t)t;
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
            u128 t = (u128)a->m[i] * b->m[j] + p[i + j] + carry;
            p[i + j] = (uint64_t)t;
            carry = (uint64_t)(t >> 64);
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

static bf bf_div_u32(const bf *a, uint32_t d)
{
    uint64_t q[4], rem = 0;
    bf r = *a;
    int s;
    if (bf_is_zero(a))
        return r;
    for (int i = 2; i >= 0; i--) {
        u128 cur = ((u128)rem << 64) | a->m[i];
        q[i + 1] = (uint64_t)(cur / d);
        rem = (uint64_t)(cur % d);
    }
    q[0] = (uint64_t)(((u128)rem << 64) / d);
    s = clz64(q[3]); /* < 32 */
    if (s) {
        r.m[2] = (q[3] << s) | (q[2] >> (64 - s));
        r.m[1] = (q[2] << s) | (q[1] >> (64 - s));
        r.m[0] = (q[1] << s) | (q[0] >> (64 - s));
        r.exp -= s;
    } else {
        r.m[2] = q[3]; r.m[1] = q[2]; r.m[0] = q[1];
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
    u128 q = ((u128)1 << 127) / b->m[2];
    x.sign = b->sign;
    x.exp = -b->exp - 1;
    x.m[2] = (q >> 64) ? ~0ull : (uint64_t)q;
    x.m[1] = x.m[0] = 0;
    for (int i = 0; i < 3; i++) {
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
    c->ex |= PE;
    if (!(r.sexp & 0x7FFF) && (c->cw & UE))
        c->ex |= UE;
    return r;
}

/* ------------------------------------------------------------------ */
/* Kernels                                                             */
/* ------------------------------------------------------------------ */

#define BF_TERM_BITS 200

static bf k_sin(const bf *r)
{
    bf r2 = bf_mul(r, r), sum = *r, term = *r;
    for (uint32_t k = 1; k < 64; k++) {
        term = bf_mul(&term, &r2);
        term = bf_div_u32(&term, (2 * k) * (2 * k + 1));
        if (bf_is_zero(&term) || term.exp < sum.exp - BF_TERM_BITS)
            break;
        sum = (k & 1) ? bf_sub(&sum, &term) : bf_add(&sum, &term);
    }
    return sum;
}

static bf k_cos(const bf *r)
{
    bf r2 = bf_mul(r, r), sum = BF_ONE, term = BF_ONE;
    for (uint32_t k = 1; k < 64; k++) {
        term = bf_mul(&term, &r2);
        term = bf_div_u32(&term, (2 * k - 1) * (2 * k));
        if (bf_is_zero(&term) || term.exp < -BF_TERM_BITS)
            break;
        sum = (k & 1) ? bf_sub(&sum, &term) : bf_add(&sum, &term);
    }
    return sum;
}

/* atan for 0 <= z <= 1 */
static bf k_atan(const bf *z)
{
    static const bf thr = { 0, -2, { 0, 0, 0xD413CCCFE7799211ull } }; /* tan(pi/8) */
    bf w = *z, w2, sum, pw;
    int reduce = 0;
    if (bf_is_zero(z))
        return *z;
    if (z->exp > thr.exp || (z->exp == thr.exp && cmp192(z->m, thr.m) > 0)) {
        bf n = bf_sub(&BF_ONE, z), d = bf_add(&BF_ONE, z);
        w = bf_div(&n, &d);
        reduce = 1;
    }
    sum = w;
    if (!bf_is_zero(&w)) {
        w2 = bf_mul(&w, &w);
        pw = w;
        for (uint32_t k = 1; k < 200; k++) {
            bf t;
            pw = bf_mul(&pw, &w2);
            t = bf_div_u32(&pw, 2 * k + 1);
            if (t.exp < sum.exp - BF_TERM_BITS)
                break;
            sum = (k & 1) ? bf_sub(&sum, &t) : bf_add(&sum, &t);
        }
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
    bf t2, sum = *t, pw = *t;
    if (bf_is_zero(t))
        return *t;
    t2 = bf_mul(t, t);
    for (uint32_t k = 1; k < 400; k++) {
        bf u;
        pw = bf_mul(&pw, &t2);
        u = bf_div_u32(&pw, 2 * k + 1);
        if (u.exp < sum.exp - BF_TERM_BITS)
            break;
        sum = bf_add(&sum, &u);
    }
    return bf_scale(sum, 1);
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
    e = bf_from_u128(ex < 0 ? (u128)(-(int64_t)ex) : (u128)ex, 0, ex < 0);
    return bf_add(&e, &l);
}

static bf k_expm1(const bf *z)
{
    bf sum = *z, term = *z;
    if (bf_is_zero(z))
        return *z;
    for (uint32_t k = 2; k < 200; k++) {
        term = bf_mul(&term, z);
        term = bf_div_u32(&term, k);
        if (term.exp < sum.exp - BF_TERM_BITS)
            break;
        sum = bf_add(&sum, &term);
    }
    return sum;
}

/* ------------------------------------------------------------------ */
/* Trigonometric functions                                             */
/* ------------------------------------------------------------------ */

/* Intel's 66-bit pi: 0xC90FDAA22168C234C * 2^-66 */
#define PI66 ((((u128)0xC) << 64) | 0x90FDAA22168C234Cull)

/* Reduce |x| modulo pi66/2 to [-pi/4, pi/4]; returns the quadrant. */
static unsigned trig_reduce(const unp *a, bf *r)
{
    int32_t e = a->exp - 16383;
    u128 rem, p = PI66;
    uint64_t q = 0;
    int shift, neg = 0;

    if (e < -1) {
        *r = bf_from_unp(a);
        r->sign = 0;
        return 0;
    }
    /* work in units of 2^-67, in which pi66/2 == PI66 */
    rem = a->sig;
    shift = e + 4;
    while (shift > 0) {
        int s = shift > 59 ? 59 : shift;
        u128 t = rem << s;
        q = (q << s) + (uint64_t)(t / p);
        rem = t % p;
        shift -= s;
    }
    if (rem > (p >> 1)) {
        rem = p - rem;
        q++;
        neg = 1;
    }
    *r = bf_from_u128(rem, -67, neg);
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
    if (a->kind >= K_QNAN) {
        *out = x87i_nan1(c, a);
        return 1;
    }
    if (a->kind == K_INF) {
        *out = x87i_invalid(c);
        return 1;
    }
    if (a->kind == K_DEN)
        c->ex |= DE;
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
    c->ex |= PE;
    if (a->exp <= 0 && (c->cw & UE))
        c->ex |= UE;
    return r;
}

#define SIN_TINY_EXP (16383 - 69)
#define TAN_TINY_EXP (16383 - 34)
#define ATAN_TINY_EXP (-41)

x87_f80 x87i_fsin(fctx *c, const unp *a, int *c2)
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

x87_f80 x87i_fcos(fctx *c, const unp *a, int *c2)
{
    x87_f80 r;
    bf v;
    if (trig_special(c, a, c2, &r))
        return r;
    if (a->kind == K_ZERO)
        return F80(0x3FFF, 0x8000000000000000ull);
    if (a->exp <= SIN_TINY_EXP) {
        c->ex |= PE;
        return F80(0x3FFF, 0x8000000000000000ull);
    }
    v = trig_eval(a, 1);
    r = bf_finish(c, &v);
    return r;
}

void x87i_fsincos(fctx *c, const unp *a, x87_f80 *s, x87_f80 *co, int *c2)
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
    c->ex |= PE;
}

x87_f80 x87i_fptan(fctx *c, const unp *a, int *c2)
{
    x87_f80 r;
    bf rr, s, co, v;
    unsigned n;
    if (trig_special(c, a, c2, &r))
        return r;
    if (a->kind == K_ZERO)
        return F80(a->sign << 15, 0);
    if (a->exp <= TAN_TINY_EXP)
        return tiny_identity(c, a);
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

x87_f80 x87i_fpatan(fctx *c, const unp *y, const unp *x)
{
    bf by, bx, z, t;
    int swap;

    if (x->kind >= K_QNAN || y->kind >= K_QNAN)
        return x87i_nan2(c, x, y);
    if (x->kind == K_DEN || y->kind == K_DEN)
        c->ex |= DE;
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

x87_f80 x87i_f2xm1(fctx *c, const unp *a)
{
    bf z, v;
    x87_f80 r;
    if (a->kind >= K_QNAN)
        return x87i_nan1(c, a);
    if (a->kind == K_ZERO)
        return F80(a->sign << 15, 0);
    if (a->kind == K_INF)
        return a->sign ? F80(0xBFFF, 0x8000000000000000ull) : F80(0x7FFF, 0x8000000000000000ull);
    if (a->kind == K_DEN)
        c->ex |= DE;
    if (a->exp >= 16383) {
        /* |x| >= 1 */
        c->ex |= PE;
        if (a->exp == 16383 && a->sig == 0x8000000000000000ull)
            return a->sign ? F80(0xBFFE, 0x8000000000000000ull) : F80(0x3FFF, 0x8000000000000000ull);
        return F80((a->sign << 15) | a->exp, a->sig); /* out of range: unchanged */
    }
    z = bf_from_unp(a);
    z = bf_mul(&z, &BF_LN2);
    v = k_expm1(&z);
    r = bf_finish(c, &v);
    return r;
}

static x87_f80 inf_signed(int sign) { return F80((sign << 15) | 0x7FFF, 0x8000000000000000ull); }

x87_f80 x87i_fyl2x(fctx *c, const unp *y, const unp *x)
{
    bf l, by, v;
    x87_f80 r;

    if (x->kind >= K_QNAN || y->kind >= K_QNAN)
        return x87i_nan2(c, x, y);
    if (x->kind == K_ZERO) {
        if (y->kind == K_ZERO)
            return x87i_invalid(c);
        if (y->kind != K_INF)
            c->ex |= ZE;
        return inf_signed(!y->sign);
    }
    if (x->sign)
        return x87i_invalid(c);
    if (x->kind == K_DEN || y->kind == K_DEN)
        c->ex |= DE;
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
        bf eps = bf_from_u128(1, l.exp - 160, 0);
        l = bf_add(&l, &eps);
    }
    by = bf_from_unp(y);
    v = bf_mul(&l, &by);
    r = bf_finish(c, &v);
    return r;
}

x87_f80 x87i_fyl2xp1(fctx *c, const unp *y, const unp *x)
{
    bf bx, l, by, v;
    x87_f80 r;

    if (x->kind >= K_QNAN || y->kind >= K_QNAN)
        return x87i_nan2(c, x, y);
    if (x->kind == K_DEN || y->kind == K_DEN)
        c->ex |= DE;
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
            c->ex |= PE;
            return F80((x->sign << 15) | x->exp, x->sig);
        }
        l = k_log2(&s);
    }
    by = bf_from_unp(y);
    v = bf_mul(&l, &by);
    r = bf_finish(c, &v);
    return r;
}
