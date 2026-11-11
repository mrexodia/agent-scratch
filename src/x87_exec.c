/* SPDX-License-Identifier: MIT
 *
 * x87 instruction execution: register stack, tags, exception delivery,
 * environment images and the ESC opcode decoder.
 */
#include "x87_internal.h"

/* ------------------------------------------------------------------ */
/* Little-endian memory helpers                                        */
/* ------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | ((uint32_t)rd16(p + 2) << 16); }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

X87_API x87_f80 x87_f80_load(const void *p)
{
    const uint8_t *b = p;
    return F80(rd16(b + 8), rd64(b));
}

X87_API void x87_f80_store(void *p, x87_f80 v)
{
    uint8_t *b = p;
    wr64(b, v.mant);
    wr16(b + 8, v.sexp);
}

/* ------------------------------------------------------------------ */
/* State helpers                                                       */
/* ------------------------------------------------------------------ */

#define CC_MASK (X87_SW_C0 | X87_SW_C1 | X87_SW_C2 | X87_SW_C3)

static inline unsigned top(const x87_state *s) { return (s->sw >> 11) & 7; }
static inline unsigned phys(const x87_state *s, unsigned i) { return (top(s) + i) & 7; }
static inline int empty(const x87_state *s, unsigned i) { return !((s->valid >> phys(s, i)) & 1); }
static inline x87_f80 *ST(x87_state *s, unsigned i) { return &s->r[phys(s, i)]; }
static inline void set_top(x87_state *s, unsigned t) { s->sw = (uint16_t)((s->sw & ~0x3800u) | ((t & 7) << 11)); }

static inline void set_st(x87_state *s, unsigned i, x87_f80 v)
{
    unsigned p = phys(s, i);
    s->r[p] = v;
    s->valid |= (uint8_t)(1u << p);
}

static inline void pop(x87_state *s)
{
    s->valid &= (uint8_t)~(1u << top(s));
    set_top(s, top(s) + 1);
}

static inline void update_es(x87_state *s)
{
    if (s->sw & ~s->cw & 0x3F)
        s->sw |= X87_SW_ES | X87_SW_B;
    else
        s->sw &= (uint16_t)~(X87_SW_ES | X87_SW_B);
}

static inline void set_c1(x87_state *s, int v)
{
    s->sw = (uint16_t)((s->sw & ~X87_SW_C1) | (v ? X87_SW_C1 : 0));
}

/* Raise a stack fault; returns non-zero if the invalid exception is masked
 * (so the instruction continues with the default response). */
static int stack_fault(x87_state *s, int overflow)
{
    s->sw |= IE | SF;
    set_c1(s, overflow);
    update_es(s);
    return s->cw & IE;
}

/* Commit the outcome of a value-level operation to the status word.
 * Returns non-zero if the result may be delivered. */
static int commit(x87_state *s, const x87_env *e)
{
    s->sw |= e->flags;
    set_c1(s, e->c1);
    update_es(s);
    return !e->inhibit;
}

X87_API uint16_t x87_tag_word(const x87_state *s)
{
    uint16_t tw = 0;
    for (int i = 0; i < 8; i++) {
        unsigned t;
        uint16_t e = s->r[i].sexp & 0x7FFF;
        if (!((s->valid >> i) & 1))
            t = 3;
        else if (e == 0 && s->r[i].mant == 0)
            t = 1;
        else if (e == 0 || e == 0x7FFF || !(s->r[i].mant >> 63))
            t = 2;
        else
            t = 0;
        tw |= (uint16_t)(t << (2 * i));
    }
    return tw;
}

static void set_tag_word(x87_state *s, uint16_t tw)
{
    s->valid = 0;
    for (int i = 0; i < 8; i++)
        if (((tw >> (2 * i)) & 3) != 3)
            s->valid |= (uint8_t)(1u << i);
}

static void set_cw(x87_state *s, uint16_t cw)
{
    s->cw = (cw & 0x1F3F) | 0x0040;
    update_es(s);
}

X87_API void x87_init(x87_state *s)
{
    s->cw = 0x037F;
    s->sw = 0;
    s->valid = 0;
    s->fop = 0;
    s->fcs = s->fds = 0;
    s->fip = s->fdp = 0;
}

X87_API int x87_pending(const x87_state *s) { return (s->sw & X87_SW_ES) != 0; }
X87_API int x87_fwait(const x87_state *s) { return x87_pending(s) ? X87_R_MF : X87_R_OK; }

/* ------------------------------------------------------------------ */
/* Environment images                                                  */
/* ------------------------------------------------------------------ */

static size_t env_size(int fmt) { return (fmt == X87_ENV_PM32 || fmt == X87_ENV_RM32) ? 28 : 14; }

static void store_env(const x87_state *s, uint8_t *p, int fmt)
{
    uint16_t tw = x87_tag_word(s);
    uint32_t lip, ldp;
    switch (fmt) {
    case X87_ENV_PM32:
        wr32(p, 0xFFFF0000u | s->cw);
        wr32(p + 4, 0xFFFF0000u | s->sw);
        wr32(p + 8, 0xFFFF0000u | tw);
        wr32(p + 12, (uint32_t)s->fip);
        wr32(p + 16, s->fcs | ((uint32_t)(s->fop & 0x7FF) << 16));
        wr32(p + 20, (uint32_t)s->fdp);
        wr32(p + 24, 0xFFFF0000u | s->fds);
        break;
    case X87_ENV_PM16:
        wr16(p, s->cw);
        wr16(p + 2, s->sw);
        wr16(p + 4, tw);
        wr16(p + 6, (uint16_t)s->fip);
        wr16(p + 8, s->fcs);
        wr16(p + 10, (uint16_t)s->fdp);
        wr16(p + 12, s->fds);
        break;
    case X87_ENV_RM32:
        lip = ((uint32_t)s->fcs << 4) + (uint32_t)s->fip;
        ldp = ((uint32_t)s->fds << 4) + (uint32_t)s->fdp;
        wr32(p, 0xFFFF0000u | s->cw);
        wr32(p + 4, 0xFFFF0000u | s->sw);
        wr32(p + 8, 0xFFFF0000u | tw);
        wr32(p + 12, 0xFFFF0000u | (lip & 0xFFFF));
        wr32(p + 16, ((lip >> 16) << 12) | (s->fop & 0x7FF));
        wr32(p + 20, 0xFFFF0000u | (ldp & 0xFFFF));
        wr32(p + 24, (ldp >> 16) << 12);
        break;
    default: /* X87_ENV_RM16 */
        lip = ((uint32_t)s->fcs << 4) + (uint32_t)s->fip;
        ldp = ((uint32_t)s->fds << 4) + (uint32_t)s->fdp;
        wr16(p, s->cw);
        wr16(p + 2, s->sw);
        wr16(p + 4, tw);
        wr16(p + 6, (uint16_t)lip);
        wr16(p + 8, (uint16_t)((((lip >> 16) & 0xF) << 12) | (s->fop & 0x7FF)));
        wr16(p + 10, (uint16_t)ldp);
        wr16(p + 12, (uint16_t)(((ldp >> 16) & 0xF) << 12));
        break;
    }
}

static void load_env(x87_state *s, const uint8_t *p, int fmt)
{
    uint32_t hi;
    switch (fmt) {
    case X87_ENV_PM32:
        s->sw = rd16(p + 4);
        set_tag_word(s, rd16(p + 8));
        s->fip = rd32(p + 12);
        s->fcs = rd16(p + 16);
        s->fop = rd16(p + 18) & 0x7FF;
        s->fdp = rd32(p + 20);
        s->fds = rd16(p + 24);
        set_cw(s, rd16(p));
        break;
    case X87_ENV_PM16:
        s->sw = rd16(p + 2);
        set_tag_word(s, rd16(p + 4));
        s->fip = rd16(p + 6);
        s->fcs = rd16(p + 8);
        s->fdp = rd16(p + 10);
        s->fds = rd16(p + 12);
        set_cw(s, rd16(p));
        break;
    case X87_ENV_RM32:
        s->sw = rd16(p + 4);
        set_tag_word(s, rd16(p + 8));
        hi = rd32(p + 16);
        s->fip = rd16(p + 12) | (((hi >> 12) & 0xFFFF) << 16);
        s->fop = hi & 0x7FF;
        hi = rd32(p + 24);
        s->fdp = rd16(p + 20) | (((hi >> 12) & 0xFFFF) << 16);
        s->fcs = s->fds = 0;
        set_cw(s, rd16(p));
        break;
    default:
        s->sw = rd16(p + 2);
        set_tag_word(s, rd16(p + 4));
        hi = rd16(p + 8);
        s->fip = rd16(p + 6) | ((hi >> 12) << 16);
        s->fop = hi & 0x7FF;
        s->fdp = rd16(p + 10) | ((uint32_t)(rd16(p + 12) >> 12) << 16);
        s->fcs = s->fds = 0;
        set_cw(s, rd16(p));
        break;
    }
}

X87_API void x87_fxsave(const x87_state *s, uint8_t *a, int rex_w)
{
    wr16(a, s->cw);
    wr16(a + 2, s->sw);
    a[4] = s->valid;
    a[5] = 0;
    wr16(a + 6, s->fop & 0x7FF);
    if (rex_w) {
        wr64(a + 8, s->fip);
        wr64(a + 16, s->fdp);
    } else {
        wr32(a + 8, (uint32_t)s->fip);
        wr16(a + 12, s->fcs);
        wr16(a + 14, 0);
        wr32(a + 16, (uint32_t)s->fdp);
        wr16(a + 20, s->fds);
        wr16(a + 22, 0);
    }
    for (unsigned i = 0; i < 8; i++) {
        uint8_t *p = a + 32 + 16 * i;
        x87_f80_store(p, s->r[(top(s) + i) & 7]);
        for (int k = 10; k < 16; k++)
            p[k] = 0;
    }
}

X87_API void x87_fxrstor(x87_state *s, const uint8_t *a, int rex_w)
{
    s->sw = rd16(a + 2);
    s->valid = a[4];
    s->fop = rd16(a + 6) & 0x7FF;
    if (rex_w) {
        s->fip = rd64(a + 8);
        s->fdp = rd64(a + 16);
        s->fcs = s->fds = 0;
    } else {
        s->fip = rd32(a + 8);
        s->fcs = rd16(a + 12);
        s->fdp = rd32(a + 16);
        s->fds = rd16(a + 20);
    }
    for (unsigned i = 0; i < 8; i++)
        s->r[(top(s) + i) & 7] = x87_f80_load(a + 32 + 16 * i);
    set_cw(s, rd16(a));
}

X87_API uint64_t x87_mmx_read(const x87_state *s, int reg) { return s->r[reg & 7].mant; }

X87_API void x87_mmx_write(x87_state *s, int reg, uint64_t value)
{
    s->r[reg & 7] = F80(0xFFFF, value);
    s->valid = 0xFF;
    set_top(s, 0);
}

X87_API void x87_emms(x87_state *s) { s->valid = 0; }

/* ------------------------------------------------------------------ */
/* Instruction bodies                                                  */
/* ------------------------------------------------------------------ */

/* Push v; handles stack overflow. */
static void push_value(x87_state *s, x87_f80 v)
{
    if (!empty(s, 7)) {
        if (!stack_fault(s, 1))
            return;
        v = F80_DEFAULT_NAN;
    } else {
        set_c1(s, 0);
    }
    set_top(s, top(s) + 7);
    set_st(s, 0, v);
}

/* Stack underflow on an instruction whose result goes to ST(dst). */
static void underflow_to(x87_state *s, unsigned dst, int npop)
{
    if (stack_fault(s, 0)) {
        set_st(s, dst, F80_DEFAULT_NAN);
        while (npop--)
            pop(s);
    }
}

/* Stack fault on an instruction that replaces ST(0) and pushes. */
static void fault_push2(x87_state *s, int overflow)
{
    if (stack_fault(s, overflow)) {
        set_st(s, 0, F80_DEFAULT_NAN);
        set_top(s, top(s) + 7);
        set_st(s, 0, F80_DEFAULT_NAN);
    }
}

static void do_arith(x87_state *s, int fn, const x87_operand *b, int b_empty, unsigned dst, int npop)
{
    x87_env e = { s->cw, 0, 0, 0 };
    x87_operand a;
    x87_f80 r;

    if (empty(s, 0) || b_empty) {
        underflow_to(s, dst, npop);
        return;
    }
    a = x87_operand_f80(*ST(s, 0));
    switch (fn) {
    case 0:  r = x87_add(&a, b, &e); break;
    case 1:  r = x87_mul(&a, b, &e); break;
    case 4:  r = x87_sub(&a, b, &e); break;
    case 5:  r = x87_sub(b, &a, &e); break;
    case 6:  r = x87_div(&a, b, &e); break;
    default: r = x87_div(b, &a, &e); break;
    }
    if (!commit(s, &e))
        return;
    set_st(s, dst, r);
    if (npop)
        pop(s);
}

/* flags: bit0 quiet (FUCOM), bit1 result to EFLAGS */
static void do_compare(x87_state *s, const x87_operand *b, int b_empty, int flags, int npop, x87_insn *in)
{
    x87_env e = { s->cw, 0, 0, 0 };
    int r = X87_CMP_UN;
    static const uint16_t cc[4] = { X87_SW_C3, 0, X87_SW_C0, X87_SW_C3 | X87_SW_C2 | X87_SW_C0 };
    static const uint32_t ef[4] = { 0x40, 0, 0x01, 0x45 };

    if (empty(s, 0) || b_empty) {
        s->sw |= IE | SF;
        set_c1(s, 0);
        e.inhibit = !(s->cw & IE);
    } else {
        x87_operand a = x87_operand_f80(*ST(s, 0));
        r = x87_cmp(&a, b, flags & 1, &e);
        s->sw |= e.flags;
        if (!(flags & 2))
            set_c1(s, 0); /* F[U]COMI[P] leave C1 alone */
    }
    update_es(s);
    if (flags & 2)
        in->eflags = (in->eflags & ~0x8D5u) | ef[r];
    else
        s->sw = (uint16_t)((s->sw & ~(X87_SW_C0 | X87_SW_C2 | X87_SW_C3)) | cc[r]);
    if (e.inhibit)
        return;
    while (npop--)
        pop(s);
}

/* FLD m32real / m64real */
static void do_load(x87_state *s, x87_f80 v, const x87_env *e)
{
    if (!empty(s, 7)) {
        push_value(s, F80_DEFAULT_NAN);
        return;
    }
    if (commit(s, e))
        push_value(s, v);
}

static void do_fprem(x87_state *s, int ieee)
{
    x87_env e = { s->cw, 0, 0, 0 };
    uint16_t cc, mask;
    x87_f80 r;

    if (empty(s, 0) || empty(s, 1)) {
        s->sw &= (uint16_t)~X87_SW_C2;
        underflow_to(s, 0, 0);
        return;
    }
    r = x87_prem(*ST(s, 0), *ST(s, 1), ieee, &e, &cc, &mask);
    s->sw = (uint16_t)((s->sw & ~mask) | cc | e.flags);
    update_es(s);
    if (!e.inhibit)
        set_st(s, 0, r);
}

static void do_fscale(x87_state *s)
{
    x87_env e = { s->cw, 0, 0, 0 };
    x87_f80 r;
    if (empty(s, 0) || empty(s, 1)) {
        underflow_to(s, 0, 0);
        return;
    }
    r = x87_scale(*ST(s, 0), *ST(s, 1), &e);
    if (commit(s, &e))
        set_st(s, 0, r);
}

static void do_fxtract(x87_state *s)
{
    x87_env e = { s->cw, 0, 0, 0 };
    x87_f80 sig, ex;

    if (empty(s, 0)) {
        fault_push2(s, 0);
        return;
    }
    if (!empty(s, 7)) {
        fault_push2(s, 1);
        return;
    }
    ex = x87_xtract(*ST(s, 0), &sig, &e);
    if (!commit(s, &e))
        return;
    set_st(s, 0, ex);
    set_top(s, top(s) + 7);
    set_st(s, 0, sig);
}

/* Unary operation on ST(0) with the usual exception protocol. */
typedef x87_f80 (*unary_fn)(x87_f80, x87_env *);

static void do_unary(x87_state *s, unary_fn fn)
{
    x87_env e = { s->cw, 0, 0, 0 };
    x87_f80 r;
    if (empty(s, 0)) {
        underflow_to(s, 0, 0);
        return;
    }
    r = fn(*ST(s, 0), &e);
    if (commit(s, &e))
        set_st(s, 0, r);
}

/* FSIN / FCOS / FPTAN / FSINCOS: mode 0 sin, 1 cos, 2 tan, 3 sincos */
static void do_trig(x87_state *s, int mode)
{
    x87_env e = { s->cw, 0, 0, 0 };
    x87_f80 a, r, r2 = F80(0, 0);
    int c2 = 0, push = mode >= 2;

    s->sw &= (uint16_t)~X87_SW_C2;
    if (empty(s, 0)) {
        if (push)
            fault_push2(s, 0);
        else
            underflow_to(s, 0, 0);
        return;
    }
    if (push && !empty(s, 7)) {
        fault_push2(s, 1);
        return;
    }
    a = *ST(s, 0);
    switch (mode) {
    case 0:  r = x87_sin(a, &e, &c2); break;
    case 1:  r = x87_cos(a, &e, &c2); break;
    case 2:  r = x87_tan(a, &r2, &e, &c2); break;
    default: r = x87_sincos(a, &r2, &e, &c2); break;
    }
    if (c2) {
        s->sw |= X87_SW_C2;
        set_c1(s, 0);
        return;
    }
    if (!commit(s, &e))
        return;
    set_st(s, 0, r);
    if (push) {
        set_top(s, top(s) + 7);
        set_st(s, 0, r2);
    }
}

/* FPATAN / FYL2X / FYL2XP1: ST(1) = f(ST(1), ST(0)); pop */
typedef x87_f80 (*binary_fn)(x87_f80, x87_f80, x87_env *);

static void do_binary_pop(x87_state *s, binary_fn fn)
{
    x87_env e = { s->cw, 0, 0, 0 };
    x87_f80 r;
    if (empty(s, 0) || empty(s, 1)) {
        underflow_to(s, 1, 1);
        return;
    }
    r = fn(*ST(s, 1), *ST(s, 0), &e);
    if (!commit(s, &e))
        return;
    set_st(s, 1, r);
    pop(s);
}

static void do_fxam(x87_state *s)
{
    uint16_t cc = empty(s, 0) ? (X87_SW_C3 | X87_SW_C0) : x87_xam(*ST(s, 0));
    if (ST(s, 0)->sexp & 0x8000)
        cc |= X87_SW_C1;
    s->sw = (uint16_t)((s->sw & ~CC_MASK) | cc);
}

/* Store ST(0) to memory. kind: 0 m32, 1 m64, 2 m80, 3 i16, 4 i32, 5 i64, 6 bcd; */
static int do_store(x87_state *s, int kind, int trunc, int npop, uint8_t *mem)
{
    static const uint8_t ibits[3] = { 16, 32, 64 };
    x87_env e = { s->cw, 0, 0, 0 };
    x87_f80 v;
    uint64_t out = 0;
    uint8_t bcd[10];

    if (empty(s, 0)) {
        if (!stack_fault(s, 0))
            return 0;
        switch (kind) {
        case 0: wr32(mem, 0xFFC00000u); break;
        case 1: wr64(mem, 0xFFF8000000000000ull); break;
        case 2: x87_f80_store(mem, F80_DEFAULT_NAN); break;
        case 3: wr16(mem, 0x8000); break;
        case 4: wr32(mem, 0x80000000u); break;
        case 5: wr64(mem, 0x8000000000000000ull); break;
        default:
            wr64(mem, 0xC000000000000000ull);
            wr16(mem + 8, 0xFFFF);
            break;
        }
        if (npop)
            pop(s);
        return X87_R_MEM_WRITE;
    }
    v = *ST(s, 0);
    switch (kind) {
    case 0: out = x87_to_f32(v, &e); break;
    case 1: out = x87_to_f64(v, &e); break;
    case 2: break;
    case 6: x87_to_bcd(v, bcd, &e); break;
    default: out = (uint64_t)x87_to_int(v, ibits[kind - 3], trunc, &e); break;
    }
    if (!commit(s, &e))
        return 0;
    switch (kind) {
    case 0: wr32(mem, (uint32_t)out); break;
    case 1: wr64(mem, out); break;
    case 2: x87_f80_store(mem, v); break;
    case 3: wr16(mem, (uint16_t)out); break;
    case 4: wr32(mem, (uint32_t)out); break;
    case 5: wr64(mem, out); break;
    default:
        for (int i = 0; i < 10; i++)
            mem[i] = bcd[i];
        break;
    }
    if (npop)
        pop(s);
    return X87_R_MEM_WRITE;
}

static void do_fxch(x87_state *s, unsigned i)
{
    x87_f80 a, b;
    if (empty(s, 0) || empty(s, i)) {
        if (!stack_fault(s, 0))
            return;
        if (empty(s, 0))
            set_st(s, 0, F80_DEFAULT_NAN);
        if (empty(s, i))
            set_st(s, i, F80_DEFAULT_NAN);
    } else {
        set_c1(s, 0);
    }
    a = *ST(s, 0);
    b = *ST(s, i);
    *ST(s, 0) = b;
    *ST(s, i) = a;
}

/* FST/FSTP ST(i) */
static void do_fst_reg(x87_state *s, unsigned i, int npop)
{
    if (empty(s, 0)) {
        underflow_to(s, i, npop);
        return;
    }
    set_c1(s, 0);
    set_st(s, i, *ST(s, 0));
    if (npop)
        pop(s);
}

static void do_fcmov(x87_state *s, unsigned i, int cond)
{
    if (empty(s, 0) || empty(s, i)) {
        underflow_to(s, 0, 0);
        return;
    }
    if (cond)
        *ST(s, 0) = *ST(s, i);
}

static void do_sign(x87_state *s, int chs)
{
    if (empty(s, 0)) {
        underflow_to(s, 0, 0);
        return;
    }
    set_c1(s, 0);
    if (chs)
        ST(s, 0)->sexp ^= 0x8000;
    else
        ST(s, 0)->sexp &= 0x7FFF;
}

/* ------------------------------------------------------------------ */
/* Decoder                                                             */
/* ------------------------------------------------------------------ */

X87_API size_t x87_mem_operand(uint8_t op, uint8_t modrm, int env_fmt, int *access)
{
    static const uint8_t arith_sz[8] = { 4, 0, 4, 0, 8, 0, 2, 0 };
    unsigned reg = (modrm >> 3) & 7;
    int acc = X87_MEM_NONE;
    size_t sz = 0;

    if (op < 0xD8 || op > 0xDF || modrm >= 0xC0)
        goto out;
    if (!(op & 1)) {
        sz = arith_sz[op & 7];
        acc = X87_MEM_READ;
        goto out;
    }
    switch (op) {
    case 0xD9:
        switch (reg) {
        case 0: sz = 4; acc = X87_MEM_READ; break;
        case 2: case 3: sz = 4; acc = X87_MEM_WRITE; break;
        case 4: sz = env_size(env_fmt); acc = X87_MEM_READ; break;
        case 5: sz = 2; acc = X87_MEM_READ; break;
        case 6: sz = env_size(env_fmt); acc = X87_MEM_WRITE; break;
        case 7: sz = 2; acc = X87_MEM_WRITE; break;
        default: break;
        }
        break;
    case 0xDB:
        switch (reg) {
        case 0: sz = 4; acc = X87_MEM_READ; break;
        case 1: case 2: case 3: sz = 4; acc = X87_MEM_WRITE; break;
        case 5: sz = 10; acc = X87_MEM_READ; break;
        case 7: sz = 10; acc = X87_MEM_WRITE; break;
        default: break;
        }
        break;
    case 0xDD:
        switch (reg) {
        case 0: sz = 8; acc = X87_MEM_READ; break;
        case 1: case 2: case 3: sz = 8; acc = X87_MEM_WRITE; break;
        case 4: sz = env_size(env_fmt) + 80; acc = X87_MEM_READ; break;
        case 6: sz = env_size(env_fmt) + 80; acc = X87_MEM_WRITE; break;
        case 7: sz = 2; acc = X87_MEM_WRITE; break;
        default: break;
        }
        break;
    default: /* 0xDF */
        switch (reg) {
        case 0: sz = 2; acc = X87_MEM_READ; break;
        case 1: case 2: case 3: sz = 2; acc = X87_MEM_WRITE; break;
        case 4: sz = 10; acc = X87_MEM_READ; break;
        case 5: sz = 8; acc = X87_MEM_READ; break;
        case 6: sz = 10; acc = X87_MEM_WRITE; break;
        case 7: sz = 8; acc = X87_MEM_WRITE; break;
        }
        break;
    }
out:
    if (access)
        *access = acc;
    return sz;
}

/* Classification used for the #MF check and FIP/FDP/FOP update. */
enum { CL_INVALID, CL_NORMAL, CL_CONTROL_WAIT, CL_CONTROL_NOWAIT };

static int classify(uint8_t op, uint8_t modrm)
{
    unsigned reg = (modrm >> 3) & 7;
    if (modrm < 0xC0) {
        switch (op) {
        case 0xD9:
            if (reg == 1) return CL_INVALID;
            if (reg == 4 || reg == 5) return CL_CONTROL_WAIT;
            if (reg >= 6) return CL_CONTROL_NOWAIT;
            return CL_NORMAL;
        case 0xDB:
            return (reg == 4 || reg == 6) ? CL_INVALID : CL_NORMAL;
        case 0xDD:
            if (reg == 5) return CL_INVALID;
            if (reg == 4) return CL_CONTROL_WAIT;
            if (reg >= 6) return CL_CONTROL_NOWAIT;
            return CL_NORMAL;
        default:
            return CL_NORMAL;
        }
    }
    switch (op) {
    case 0xD8:
    case 0xDC:
    case 0xDE:
        if (op == 0xDE && reg == 3)
            return modrm == 0xD9 ? CL_NORMAL : CL_INVALID;
        return CL_NORMAL;
    case 0xD9:
        if (reg == 2) return modrm == 0xD0 ? CL_NORMAL : CL_INVALID;
        if (modrm == 0xE2 || modrm == 0xE3 || modrm == 0xE6 || modrm == 0xE7 || modrm == 0xEF)
            return CL_INVALID;
        return CL_NORMAL;
    case 0xDA:
        if (reg <= 3) return CL_NORMAL;
        return modrm == 0xE9 ? CL_NORMAL : CL_INVALID;
    case 0xDB:
        if (reg <= 3 || reg == 5 || reg == 6) return CL_NORMAL;
        if (reg == 4 && modrm <= 0xE4) return CL_CONTROL_NOWAIT;
        return CL_INVALID;
    case 0xDD:
        return reg <= 5 ? CL_NORMAL : CL_INVALID;
    default: /* 0xDF */
        if (reg <= 3 || reg == 5 || reg == 6) return CL_NORMAL;
        if (modrm == 0xE0) return CL_CONTROL_NOWAIT;
        return CL_INVALID;
    }
}

static int exec_mem(x87_state *s, x87_insn *in)
{
    uint8_t *m = in->mem;
    unsigned reg = (in->modrm >> 3) & 7;
    x87_operand b;
    x87_env e = { s->cw, 0, 0, 0 };

    if (!(in->op & 1)) {
        /* D8/DA/DC/DE: arithmetic with m32real/m32int/m64real/m16int */
        switch (in->op) {
        case 0xD8: b = x87_operand_f32(rd32(m)); break;
        case 0xDA: b = x87_operand_int((int32_t)rd32(m)); break;
        case 0xDC: b = x87_operand_f64(rd64(m)); break;
        default:   b = x87_operand_int((int16_t)rd16(m)); break;
        }
        if (reg == 2 || reg == 3)
            do_compare(s, &b, 0, 0, reg == 3, in);
        else
            do_arith(s, (int)reg, &b, 0, 0, 0);
        return 0;
    }
    switch (in->op) {
    case 0xD9:
        switch (reg) {
        case 0: { x87_f80 v = x87_from_f32(rd32(m), &e); do_load(s, v, &e); return 0; }
        case 2: return do_store(s, 0, 0, 0, m);
        case 3: return do_store(s, 0, 0, 1, m);
        case 4: load_env(s, m, in->env_fmt); return 0;
        case 5: set_cw(s, rd16(m)); return 0;
        case 6:
            store_env(s, m, in->env_fmt);
            s->cw |= 0x3F;
            update_es(s);
            return X87_R_MEM_WRITE;
        default: wr16(m, s->cw); return X87_R_MEM_WRITE;
        }
    case 0xDB:
        switch (reg) {
        case 0: break;
        case 1: return do_store(s, 4, 1, 1, m);
        case 2: return do_store(s, 4, 0, 0, m);
        case 3: return do_store(s, 4, 0, 1, m);
        case 5: push_value(s, x87_f80_load(m)); return 0;
        default: return do_store(s, 2, 0, 1, m);
        }
        break;
    case 0xDD:
        switch (reg) {
        case 0: { x87_f80 v = x87_from_f64(rd64(m), &e); do_load(s, v, &e); return 0; }
        case 1: return do_store(s, 5, 1, 1, m);
        case 2: return do_store(s, 1, 0, 0, m);
        case 3: return do_store(s, 1, 0, 1, m);
        case 4: {
            size_t es = env_size(in->env_fmt);
            load_env(s, m, in->env_fmt);
            for (unsigned i = 0; i < 8; i++)
                s->r[(top(s) + i) & 7] = x87_f80_load(m + es + 10 * i);
            return 0;
        }
        case 6: {
            size_t es = env_size(in->env_fmt);
            store_env(s, m, in->env_fmt);
            for (unsigned i = 0; i < 8; i++)
                x87_f80_store(m + es + 10 * i, s->r[(top(s) + i) & 7]);
            x87_init(s);
            return X87_R_MEM_WRITE;
        }
        default: wr16(m, s->sw); return X87_R_MEM_WRITE;
        }
    default: /* 0xDF */
        switch (reg) {
        case 0: break;
        case 1: return do_store(s, 3, 1, 1, m);
        case 2: return do_store(s, 3, 0, 0, m);
        case 3: return do_store(s, 3, 0, 1, m);
        case 4: push_value(s, x87_from_bcd(m)); return 0;
        case 5: break;
        case 6: return do_store(s, 6, 0, 1, m);
        default: return do_store(s, 5, 0, 1, m);
        }
        break;
    }
    /* FILD m16/m32/m64 */
    {
        int64_t v = in->op == 0xDB ? (int32_t)rd32(m) : reg == 0 ? (int16_t)rd16(m) : (int64_t)rd64(m);
        push_value(s, x87_from_int(v));
    }
    return 0;
}

static int exec_reg(x87_state *s, x87_insn *in)
{
    unsigned reg = (in->modrm >> 3) & 7, i = in->modrm & 7;
    x87_operand b = x87_operand_f80(*ST(s, i));
    int be = empty(s, i);
    uint32_t ef = in->eflags;

    switch (in->op) {
    case 0xD8:
        if (reg == 2 || reg == 3)
            do_compare(s, &b, be, 0, reg == 3, in);
        else
            do_arith(s, (int)reg, &b, be, 0, 0);
        return 0;
    case 0xDC:
        if (reg == 2 || reg == 3)
            do_compare(s, &b, be, 0, reg == 3, in);
        else
            do_arith(s, (int)reg, &b, be, i, 0);
        return 0;
    case 0xDE:
        if (reg == 2)
            do_compare(s, &b, be, 0, 1, in);
        else if (reg == 3)
            do_compare(s, &b, be, 0, 2, in);
        else
            do_arith(s, (int)reg, &b, be, i, 1);
        return 0;
    case 0xD9:
        switch (reg) {
        case 0:
            if (be) {
                if (stack_fault(s, 0)) {
                    set_top(s, top(s) + 7);
                    set_st(s, 0, F80_DEFAULT_NAN);
                }
            } else {
                push_value(s, *ST(s, i));
            }
            return 0;
        case 1: do_fxch(s, i); return 0;
        case 2: return 0; /* FNOP */
        case 3:
            /* undocumented FSTP alias that does not check for stack underflow */
            if (!empty(s, 0))
                set_st(s, i, *ST(s, 0));
            pop(s);
            set_c1(s, 0);
            return 0;
        default: break;
        }
        switch (in->modrm) {
        case 0xE0: do_sign(s, 1); break;
        case 0xE1: do_sign(s, 0); break;
        case 0xE4: {
            x87_operand z = x87_operand_int(0);
            do_compare(s, &z, 0, 0, 0, in);
            break;
        }
        case 0xE5: do_fxam(s); break;
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE:
            push_value(s, x87_const(in->modrm - 0xE8, s->cw));
            break;
        case 0xF0: do_unary(s, x87_2xm1); break;
        case 0xF1: do_binary_pop(s, x87_yl2x); break;
        case 0xF2: do_trig(s, 2); break;
        case 0xF3: do_binary_pop(s, x87_atan2); break;
        case 0xF4: do_fxtract(s); break;
        case 0xF5: do_fprem(s, 1); break;
        case 0xF6: set_top(s, top(s) + 7); set_c1(s, 0); break;
        case 0xF7: set_top(s, top(s) + 1); set_c1(s, 0); break;
        case 0xF8: do_fprem(s, 0); break;
        case 0xF9: do_binary_pop(s, x87_yl2xp1); break;
        case 0xFA: do_unary(s, x87_sqrt); break;
        case 0xFB: do_trig(s, 3); break;
        case 0xFC: do_unary(s, x87_rndint); break;
        case 0xFD: do_fscale(s); break;
        case 0xFE: do_trig(s, 0); break;
        default:   do_trig(s, 1); break;
        }
        return 0;
    case 0xDA:
        if (reg == 5) {
            b = x87_operand_f80(*ST(s, 1));
            do_compare(s, &b, empty(s, 1), 1, 2, in);
            return 0;
        }
        do_fcmov(s, i, reg == 0 ? (ef & 1) : reg == 1 ? (ef & 0x40) : reg == 2 ? (ef & 0x41) : (ef & 4));
        return 0;
    case 0xDB:
        switch (reg) {
        case 4:
            if (in->modrm == 0xE2) {
                s->sw &= 0x7F00;
            } else if (in->modrm == 0xE3) {
                x87_init(s);
            }
            return 0;
        case 5: do_compare(s, &b, be, 3, 0, in); return X87_R_EFLAGS;
        case 6: do_compare(s, &b, be, 2, 0, in); return X87_R_EFLAGS;
        default:
            do_fcmov(s, i, !(reg == 0 ? (ef & 1) : reg == 1 ? (ef & 0x40) : reg == 2 ? (ef & 0x41) : (ef & 4)));
            return 0;
        }
    case 0xDD:
        switch (reg) {
        case 0: s->valid &= (uint8_t)~(1u << phys(s, i)); set_c1(s, 0); return 0;
        case 1: do_fxch(s, i); return 0;
        case 2: do_fst_reg(s, i, 0); return 0;
        case 3: do_fst_reg(s, i, 1); return 0;
        case 4: do_compare(s, &b, be, 1, 0, in); return 0;
        default: do_compare(s, &b, be, 1, 1, in); return 0;
        }
    default: /* 0xDF */
        switch (reg) {
        case 0:
            s->valid &= (uint8_t)~(1u << phys(s, i));
            pop(s);
            set_c1(s, 0);
            return 0;
        case 1: do_fxch(s, i); return 0;
        case 2: case 3: do_fst_reg(s, i, 1); return 0;
        case 4: in->ax = s->sw; return X87_R_AX;
        case 5: do_compare(s, &b, be, 3, 1, in); return X87_R_EFLAGS;
        default: do_compare(s, &b, be, 2, 1, in); return X87_R_EFLAGS;
        }
    }
}

X87_API int x87_exec(x87_state *s, x87_insn *in)
{
    int cl, r;

    if (in->op < 0xD8 || in->op > 0xDF)
        return X87_R_UD;
    cl = classify(in->op, in->modrm);
    if (cl == CL_INVALID)
        return X87_R_UD;
    if (cl != CL_CONTROL_NOWAIT && (s->sw & X87_SW_ES))
        return X87_R_MF;
    r = in->modrm < 0xC0 ? exec_mem(s, in) : exec_reg(s, in);
    if (cl == CL_NORMAL) {
        s->fop = (uint16_t)(((in->op & 7) << 8) | in->modrm);
        s->fip = in->ip;
        s->fcs = in->cs;
        if (in->modrm < 0xC0) {
            s->fdp = in->dp;
            s->fds = in->ds;
        }
    }
    return r;
}
