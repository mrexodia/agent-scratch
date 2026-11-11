/* SPDX-License-Identifier: MIT
 *
 * Public value-level operations (see x87_ops.h): each wraps the arithmetic
 * core and resolves which exceptions inhibit the result.
 */
#include "x87_internal.h"

#define PRE (IE | ZE | DE) /* faults detected before the computation */

static inline void begin(x87_env *e)
{
    e->flags = 0;
    e->c1 = 0;
    e->inhibit = 0;
}

X87_API x87_operand x87_operand_f80(x87_f80 v) { return x87i_unpack(v); }
X87_API x87_operand x87_operand_f32(uint32_t bits) { return x87i_from_f32(bits); }
X87_API x87_operand x87_operand_f64(uint64_t bits) { return x87i_from_f64(bits); }
X87_API x87_operand x87_operand_int(int64_t v) { return x87i_from_int(v); }

X87_API x87_f80 x87_add(const x87_operand *a, const x87_operand *b, x87_env *e)
{
    x87_f80 r;
    begin(e);
    r = x87i_add(e, a, b, 0);
    x87i_resolve(e, PRE);
    return r;
}

X87_API x87_f80 x87_sub(const x87_operand *a, const x87_operand *b, x87_env *e)
{
    x87_f80 r;
    begin(e);
    r = x87i_add(e, a, b, 1);
    x87i_resolve(e, PRE);
    return r;
}

X87_API x87_f80 x87_mul(const x87_operand *a, const x87_operand *b, x87_env *e)
{
    x87_f80 r;
    begin(e);
    r = x87i_mul(e, a, b);
    x87i_resolve(e, PRE);
    return r;
}

X87_API x87_f80 x87_div(const x87_operand *a, const x87_operand *b, x87_env *e)
{
    x87_f80 r;
    begin(e);
    r = x87i_div(e, a, b);
    x87i_resolve(e, PRE);
    return r;
}

X87_API x87_f80 x87_sqrt(x87_f80 a, x87_env *e)
{
    unp u = x87i_unpack(a);
    x87_f80 r;
    begin(e);
    r = x87i_sqrt(e, &u);
    x87i_resolve(e, PRE);
    return r;
}

X87_API x87_f80 x87_rndint(x87_f80 a, x87_env *e)
{
    unp u = x87i_unpack(a);
    x87_f80 r;
    begin(e);
    r = x87i_rndint(e, &u);
    x87i_resolve(e, PRE);
    return r;
}

X87_API x87_f80 x87_scale(x87_f80 a, x87_f80 b, x87_env *e)
{
    x87_f80 r;
    begin(e);
    r = x87i_scale(e, a, b);
    x87i_resolve(e, PRE);
    return r;
}

X87_API x87_f80 x87_prem(x87_f80 a, x87_f80 b, int ieee, x87_env *e, uint16_t *cc, uint16_t *cc_mask)
{
    uint64_t q;
    int status;
    x87_f80 r;
    begin(e);
    r = x87i_prem(e, a, b, ieee, &q, &status);
    e->c1 = 0; /* C1 is a quotient bit here, reported through *cc */
    *cc = 0;
    if (x87i_resolve(e, PRE) || status == 2) {
        /* fault, NaN or invalid: C0 and C3 are left alone */
        *cc_mask = X87_SW_C1 | X87_SW_C2;
    } else {
        *cc_mask = X87_SW_C0 | X87_SW_C1 | X87_SW_C2 | X87_SW_C3;
        if (status == 1) {
            *cc = X87_SW_C2;
        } else {
            if (q & 4) *cc |= X87_SW_C0;
            if (q & 2) *cc |= X87_SW_C3;
            if (q & 1) *cc |= X87_SW_C1;
        }
    }
    return r;
}

X87_API x87_f80 x87_xtract(x87_f80 a, x87_f80 *significand, x87_env *e)
{
    x87_f80 r;
    begin(e);
    r = x87i_xtract(e, a, significand);
    x87i_resolve(e, PRE);
    return r;
}

X87_API int x87_cmp(const x87_operand *a, const x87_operand *b, int quiet, x87_env *e)
{
    int r;
    begin(e);
    r = x87i_cmp(e, a, b, quiet);
    x87i_resolve(e, IE | DE);
    return r;
}

X87_API uint16_t x87_xam(x87_f80 a) { return x87i_xam(a); }

X87_API x87_f80 x87_from_f32(uint32_t bits, x87_env *e)
{
    unp u = x87i_from_f32(bits);
    x87_f80 r;
    begin(e);
    r = x87i_load(e, &u);
    x87i_resolve(e, IE); /* an unmasked #D does not prevent the load */
    return r;
}

X87_API x87_f80 x87_from_f64(uint64_t bits, x87_env *e)
{
    unp u = x87i_from_f64(bits);
    x87_f80 r;
    begin(e);
    r = x87i_load(e, &u);
    x87i_resolve(e, IE);
    return r;
}

X87_API x87_f80 x87_from_int(int64_t v)
{
    unp u = x87i_from_int(v);
    return u.kind == K_ZERO ? F80(0, 0) : F80((u.sign << 15) | u.exp, u.sig);
}

X87_API x87_f80 x87_from_bcd(const uint8_t bcd[10]) { return x87i_from_bcd(bcd); }

X87_API uint32_t x87_to_f32(x87_f80 a, x87_env *e)
{
    uint32_t r;
    begin(e);
    r = x87i_to_f32(e, a);
    x87i_resolve(e, PRE);
    return r;
}

X87_API uint64_t x87_to_f64(x87_f80 a, x87_env *e)
{
    uint64_t r;
    begin(e);
    r = x87i_to_f64(e, a);
    x87i_resolve(e, PRE);
    return r;
}

X87_API int64_t x87_to_int(x87_f80 a, int bits, int truncate, x87_env *e)
{
    unp u = x87i_unpack(a);
    int64_t r;
    begin(e);
    r = x87i_to_int(e, &u, bits, truncate);
    x87i_resolve(e, PRE);
    return r;
}

X87_API void x87_to_bcd(x87_f80 a, uint8_t bcd[10], x87_env *e)
{
    unp u = x87i_unpack(a);
    begin(e);
    x87i_to_bcd(e, &u, bcd);
    x87i_resolve(e, PRE);
}

X87_API x87_f80 x87_const(int which, uint16_t cw) { return x87i_const(which, cw); }

/* ---- transcendentals ---- */

static x87_f80 trig_done(x87_env *e, x87_f80 r, int c2, int *out_of_range)
{
    *out_of_range = c2;
    if (c2) {
        e->flags = 0;
        e->c1 = 0;
        e->inhibit = 1;
    } else {
        x87i_resolve(e, PRE);
    }
    return r;
}

X87_API x87_f80 x87_sin(x87_f80 a, x87_env *e, int *out_of_range)
{
    unp u = x87i_unpack(a);
    int c2;
    x87_f80 r;
    begin(e);
    r = x87i_fsin(e, &u, &c2);
    return trig_done(e, r, c2, out_of_range);
}

X87_API x87_f80 x87_cos(x87_f80 a, x87_env *e, int *out_of_range)
{
    unp u = x87i_unpack(a);
    int c2;
    x87_f80 r;
    begin(e);
    r = x87i_fcos(e, &u, &c2);
    return trig_done(e, r, c2, out_of_range);
}

X87_API x87_f80 x87_sincos(x87_f80 a, x87_f80 *cosine, x87_env *e, int *out_of_range)
{
    unp u = x87i_unpack(a);
    int c2;
    x87_f80 r;
    begin(e);
    x87i_fsincos(e, &u, &r, cosine, &c2);
    return trig_done(e, r, c2, out_of_range);
}

X87_API x87_f80 x87_tan(x87_f80 a, x87_f80 *one, x87_env *e, int *out_of_range)
{
    unp u = x87i_unpack(a);
    int c2;
    x87_f80 r;
    begin(e);
    r = x87i_fptan(e, &u, &c2);
    /* NaN or invalid operand: both results are the NaN */
    *one = u.kind >= K_INF ? r : F80(0x3FFF, 0x8000000000000000ull);
    return trig_done(e, r, c2, out_of_range);
}

X87_API x87_f80 x87_atan2(x87_f80 y, x87_f80 x, x87_env *e)
{
    unp uy = x87i_unpack(y), ux = x87i_unpack(x);
    x87_f80 r;
    begin(e);
    r = x87i_fpatan(e, &uy, &ux);
    x87i_resolve(e, PRE);
    return r;
}

X87_API x87_f80 x87_2xm1(x87_f80 a, x87_env *e)
{
    unp u = x87i_unpack(a);
    x87_f80 r;
    begin(e);
    r = x87i_f2xm1(e, &u);
    x87i_resolve(e, PRE);
    return r;
}

X87_API x87_f80 x87_yl2x(x87_f80 y, x87_f80 x, x87_env *e)
{
    unp uy = x87i_unpack(y), ux = x87i_unpack(x);
    x87_f80 r;
    begin(e);
    r = x87i_fyl2x(e, &uy, &ux);
    x87i_resolve(e, PRE);
    return r;
}

X87_API x87_f80 x87_yl2xp1(x87_f80 y, x87_f80 x, x87_env *e)
{
    unp uy = x87i_unpack(y), ux = x87i_unpack(x);
    x87_f80 r;
    begin(e);
    r = x87i_fyl2xp1(e, &uy, &ux);
    x87i_resolve(e, PRE);
    return r;
}
