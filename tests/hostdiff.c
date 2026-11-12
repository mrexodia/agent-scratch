/* SPDX-License-Identifier: MIT
 *
 * Differential tester: executes randomly generated x87 instructions both on
 * the host CPU (which must be x86-64) and in the emulator, starting from
 * identical randomly generated FPU states, and compares the complete
 * resulting state (FNSAVE image, memory operand, EFLAGS, AX, and whether
 * #MF / #UD was raised).
 *
 * usage: hostdiff [-s seed] [-n count] [-o op] [-m modrm] [-v maxprint]
 *   -N  skip the transcendental instructions
 *   -T  only the transcendental instructions
 *   -t  treat value-only differences of transcendentals as failures too
 *   -G file  record every case with the host's result (for tests/replay)
 *   -A  accuracy report: transcendentals on ordinary in-range operands
 *       (all exceptions masked), reporting the bit-exact rate per instruction
 */
#if !defined(__x86_64__)
#error "hostdiff needs an x86-64 host"
#endif
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "x87.h"

static uint64_t rng_s = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return rng_s * 0x2545F4914F6CDD1Dull;
}
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

/* ---------------- random values ---------------- */

static uint64_t rnd_mant(void)
{
    uint64_t m = rnd();
    switch (rn(10)) {
    case 0: m = 0; break;
    case 1: m &= 0xFFFFFFFF00000000ull; break;
    case 2: m &= ~0x7FFull; if (rn(2)) m |= 0x400; break;            /* double boundary */
    case 3: m &= ~0xFFFFFFFFFFull; if (rn(2)) m |= 0x8000000000ull; break; /* single boundary */
    case 4: m |= 0x7FFFFFFFFFFFFF00ull; break;
    case 5: m = (m & ~0xFFFull) | (rn(2) ? 0x400 : 0x3FF) | (rn(2) ? 0x800 : 0); break;
    case 6: m = (m & ~0x1FFFFFFFFFFull) | (rn(2) ? 0x8000000000ull : 0x7FFFFFFFFFull) | (rn(2) ? 0x10000000000ull : 0); break;
    case 7: m = 1ull << rn(64); break;
    default: break;
    }
    return m;
}

static x87_f80 rnd_f80(void)
{
    x87_f80 v;
    uint64_t m = rnd_mant() | 0x8000000000000000ull;
    unsigned e;
    switch (rn(32)) {
    case 0: e = 0; m = 0; break;
    case 1: e = 0x7FFF; m = 0x8000000000000000ull; break;
    case 2: e = 0x7FFF; m |= 0x4000000000000000ull; break;                     /* qnan */
    case 3: e = 0x7FFF; m &= ~0x4000000000000000ull; if (!(m << 1)) m |= 1 + rn(1000); break; /* snan */
    case 4: e = 0x7FFF; m &= 0x7FFFFFFFFFFFFFFFull; break;                     /* pseudo nan/inf */
    case 5: e = 1 + rn(0x7FFE); m &= 0x7FFFFFFFFFFFFFFFull; break;             /* unnormal */
    case 6: e = 0; m &= 0x7FFFFFFFFFFFFFFFull; break;                          /* denormal */
    case 7: e = 0; m >>= 1 + rn(63); break;                                    /* small denormal */
    case 8: e = 0; break;                                                      /* pseudo-denormal */
    case 9: e = 1 + rn(70); break;
    case 10: e = 0x7FFE - rn(70); break;
    case 11: e = 0x3FFF; m = 0x8000000000000000ull; break;
    case 12: case 13: e = 0x3FFF + rn(66); m = (rnd() | 0x8000000000000000ull) & ~((1ull << rn(64)) - 1); m |= 0x8000000000000000ull; break; /* integers */
    case 14: e = 0x3FFF + 62 + rn(4); break;
    case 15: e = 0x3FFF + 14 + rn(3); break;
    case 16: e = 0x3FFF + 30 + rn(3); break;
    case 17: e = 0x3FFF - 1022 - rn(60); break;                                /* double denormal range */
    case 18: e = 0x3FFF + 1020 + rn(6); break;
    case 19: e = 0x3FFF - 126 - rn(30); break;
    case 20: e = 0x3FFF + 125 + rn(5); break;
    case 21: e = rn(0x7FFF); break;
    case 22: e = 0x3FFF - 24576 + rn(40) - 20; break;
    case 23: e = 0x3FFF + 8192 + rn(100); break;
    default: e = 0x3FFF - 40 + rn(80); break;
    }
    v.mant = m;
    v.sexp = (uint16_t)(e | (rn(2) ? 0x8000 : 0));
    return v;
}

static uint32_t rnd_f32(void)
{
    uint32_t f = (uint32_t)rnd() & 0x7FFFFF, e;
    switch (rn(12)) {
    case 0: e = 0; f = 0; break;
    case 1: e = 0xFF; f = 0; break;
    case 2: e = 0xFF; f |= 0x400000; break;
    case 3: e = 0xFF; f &= ~0x400000u; if (!f) f = 1; break;
    case 4: e = 0; break;
    case 5: e = rn(256); break;
    case 6: e = 127; f = 0; break;
    default: e = 127 - 30 + rn(60); if (rn(3) == 0) f &= ~((1u << rn(23)) - 1); break;
    }
    return ((uint32_t)rn(2) << 31) | (e << 23) | f;
}

static uint64_t rnd_f64(void)
{
    uint64_t f = rnd() & 0xFFFFFFFFFFFFFull, e;
    switch (rn(12)) {
    case 0: e = 0; f = 0; break;
    case 1: e = 0x7FF; f = 0; break;
    case 2: e = 0x7FF; f |= 0x8000000000000ull; break;
    case 3: e = 0x7FF; f &= ~0x8000000000000ull; if (!f) f = 1; break;
    case 4: e = 0; break;
    case 5: e = rn(2048); break;
    case 6: e = 1023; f = 0; break;
    default: e = 1023 - 40 + rn(80); if (rn(3) == 0) f &= ~((1ull << rn(52)) - 1); break;
    }
    return ((uint64_t)rn(2) << 63) | (e << 52) | f;
}

static uint64_t rnd_int(void)
{
    switch (rn(8)) {
    case 0: return 0;
    case 1: return (uint64_t)(int64_t)((int)rn(21) - 10);
    case 2: return 0x8000000000000000ull >> (rn(4) * 16);
    case 3: return rnd() >> rn(64);
    case 4: return ~(rnd() >> rn(64));
    default: return rnd();
    }
}

/* ---------------- instruction selection ---------------- */

static int is_trans(unsigned op, unsigned modrm)
{
    if (op != 0xD9) return 0;
    return modrm == 0xF0 || modrm == 0xF1 || modrm == 0xF2 || modrm == 0xF3 || modrm == 0xF9 ||
           modrm == 0xFB || modrm == 0xFE || modrm == 0xFF;
}

/* ---------------- host execution ---------------- */

static uint8_t *stub;
static sigjmp_buf jb;
static volatile int cur_sig;

static void on_sig(int sig)
{
    cur_sig = sig;
    siglongjmp(jb, 1);
}

typedef struct {
    uint8_t  in_state[108];
    uint8_t  mem[128];
    uint64_t eflags;
} tcase;

typedef struct {
    uint8_t  state[108];
    uint8_t  mem[128];
    uint64_t ax, eflags;
    int      result; /* 0 ok, 1 #MF, 2 #UD */
} tresult;

static void run_host(unsigned op, unsigned modrm, int pfx66, const tcase *t, tresult *r)
{
    uint8_t *p = stub;
    uint64_t regs[2] = { 0, 0 };
    /* rdi = mem, rsi = in-state, rdx = out-state, rcx = regs, r8 = eflags */
    *p++ = 0x41; *p++ = 0x50; *p++ = 0x9D;             /* push r8; popfq */
    *p++ = 0xDD; *p++ = 0x26;                          /* frstor [rsi] */
    *p++ = 0xB8; *p++ = 0x5A; *p++ = 0x5A; *p++ = 0x5A; *p++ = 0x5A; /* mov eax, 0x5a5a5a5a */
    if (pfx66) *p++ = 0x66;
    *p++ = (uint8_t)op;
    *p++ = (uint8_t)(modrm < 0xC0 ? ((modrm & 0x38) | 0x07) : modrm);
    *p++ = 0x9C; *p++ = 0x41; *p++ = 0x59;             /* pushfq; pop r9 */
    *p++ = 0x89; *p++ = 0x01;                          /* mov [rcx], eax */
    *p++ = 0x4C; *p++ = 0x89; *p++ = 0x49; *p++ = 0x08; /* mov [rcx+8], r9 */
    *p++ = 0xDD; *p++ = 0x32;                          /* fnsave [rdx] */
    *p++ = 0x6A; *p++ = 0x02; *p++ = 0x9D;             /* push 2; popfq */
    *p++ = 0xC3;

    memcpy(r->mem, t->mem, sizeof r->mem);
    memset(r->state, 0, sizeof r->state);
    r->result = 0;
    if (sigsetjmp(jb, 1) == 0) {
        ((void (*)(void *, const void *, void *, void *, uint64_t))stub)(r->mem, t->in_state, r->state, regs, t->eflags);
    } else {
        r->result = cur_sig == SIGFPE ? 1 : 2;
        /* the instruction did not execute: capture the state as loaded */
        p = stub + 64;
        *p++ = 0xDD; *p++ = 0x26; *p++ = 0xDD; *p++ = 0x32; *p++ = 0xC3; /* frstor [rsi]; fnsave [rdx]; ret */
        ((void (*)(void *, const void *, void *))(stub + 64))(0, t->in_state, r->state);
        memcpy(r->mem, t->mem, sizeof r->mem);
        regs[0] = 0x5A5A5A5A;
        regs[1] = t->eflags;
    }
    r->ax = regs[0] & 0xFFFF;
    r->eflags = regs[1] & 0x8D5;
}

static void run_emu(unsigned op, unsigned modrm, int pfx66, const tcase *t, tresult *r)
{
    x87_state s;
    x87_insn in;
    int res;
    uint8_t tmp[108];

    x87_init(&s);
    memcpy(tmp, t->in_state, 108);
    memset(&in, 0, sizeof in);
    in.op = 0xDD; in.modrm = 0x26; in.mem = tmp; in.env_fmt = X87_ENV_PM32;
    x87_exec(&s, &in);

    memcpy(r->mem, t->mem, sizeof r->mem);
    memset(&in, 0, sizeof in);
    in.op = (uint8_t)op; in.modrm = (uint8_t)modrm; in.mem = r->mem; in.env_fmt = pfx66 ? X87_ENV_PM16 : X87_ENV_PM32;
    in.eflags = (uint32_t)t->eflags;
    in.ax = 0x5A5A;
    res = x87_exec(&s, &in);
    r->result = (res & X87_R_MF) ? 1 : (res & X87_R_UD) ? 2 : 0;
    r->ax = in.ax;
    r->eflags = in.eflags & 0x8D5;

    memset(&in, 0, sizeof in);
    in.op = 0xDD; in.modrm = 0x36; in.mem = r->state; in.env_fmt = X87_ENV_PM32;
    x87_exec(&s, &in);
}

/* ---------------- test generation ---------------- */

static void wr16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void gen_state(uint8_t *img, int allow_pending)
{
    unsigned cw, sw, tw = 0, top = rn(8);
    x87_f80 regs[8];

    cw = 0x037F;
    if (rn(4) == 0) cw = (cw & ~0x3Fu) | rn(64);
    else if (rn(8) == 0) cw &= ~(1u << rn(6));
    cw = (cw & ~0xF00u) | (rn(4) << 10) | ((rn(8) ? (rn(2) ? 3 : 2) : rn(4)) << 8);
    if (rn(16) == 0) cw ^= (unsigned)rnd() & 0xF0C0;

    sw = top << 11;
    if (rn(2)) sw |= (unsigned)rnd() & 0x4700;
    if (rn(6) == 0) sw |= (unsigned)rnd() & 0x7F;
    if (rn(16) == 0) sw |= (unsigned)rnd() & 0x8080;
    if (!allow_pending) sw &= ~0x3Fu | cw;

    for (int i = 0; i < 8; i++)
        regs[i] = rnd_f80();
    /* correlate ST1 with ST0 sometimes */
    if (rn(4) == 0) {
        regs[1] = regs[0];
        switch (rn(4)) {
        case 0: regs[1].mant ^= 1ull << rn(8); break;
        case 1: regs[1].sexp = (uint16_t)(regs[1].sexp + rn(140) - 70); break;
        case 2: regs[1].sexp ^= 0x8000; regs[1].mant += rn(3); break;
        default: break;
        }
    }
    {
        unsigned mode = rn(8);
        for (int i = 0; i < 8; i++) {
            unsigned t = 0;
            if (mode == 0) t = rn(4);
            else if (mode == 1) t = rn(2) ? 3 : 0;
            else if (mode == 2) t = 3;
            else if (mode == 3) t = (i == (int)((top + 7) & 7)) ? 3 : 0;
            else if (mode == 4) t = (i == (int)((top + 7) & 7) || i == (int)((top + 6) & 7)) ? 3 : 0;
            else t = (i == (int)top || i == (int)((top + 1) & 7)) ? 0 : (rn(4) ? 3 : 0);
            tw |= t << (2 * i);
        }
    }
    memset(img, 0, 108);
    wr16(img, cw); wr16(img + 2, 0xFFFF);
    wr16(img + 4, sw); wr16(img + 6, 0xFFFF);
    wr16(img + 8, tw); wr16(img + 10, 0xFFFF);
    for (unsigned i = 0; i < 8; i++)
        x87_f80_store(img + 28 + 10 * i, regs[(top + i) & 7]);
}

/* Ordinary operands for the accuracy report. */
static x87_f80 norm_f80(int emin, int emax, int allow_neg)
{
    x87_f80 v;
    v.mant = rnd() | 0x8000000000000000ull;
    v.sexp = (uint16_t)(0x3FFF + emin + (int)rn((unsigned)(emax - emin + 1)));
    if (allow_neg && rn(2)) v.sexp |= 0x8000;
    return v;
}

static void gen_accuracy_state(unsigned modrm, uint8_t *img)
{
    x87_f80 st0, st1 = norm_f80(-10, 10, 1);
    memset(img, 0, 108);
    wr16(img, 0x037F | (rn(4) << 10)); wr16(img + 2, 0xFFFF);
    wr16(img + 4, 0); wr16(img + 6, 0xFFFF);
    wr16(img + 8, 0xFFF0); wr16(img + 10, 0xFFFF);
    switch (modrm) {
    case 0xF0: st0 = norm_f80(-20, -1, 1); break;   /* f2xm1: |x| < 1 */
    case 0xF1: st0 = norm_f80(-10, 10, 0); break;   /* fyl2x: x > 0 */
    case 0xF3: st0 = norm_f80(-10, 10, 1); break;   /* fpatan */
    case 0xF9: st0 = norm_f80(-20, -2, 1); break;   /* fyl2xp1: |x| < 0.29 */
    default:   st0 = rn(2) ? norm_f80(-20, 1, 1) : norm_f80(2, 40, 1); break; /* trig */
    }
    x87_f80_store(img + 28, st0);
    x87_f80_store(img + 38, st1);
}

static void gen_mem(unsigned op, unsigned modrm, uint8_t *mem)
{
    unsigned reg = (modrm >> 3) & 7;
    for (int i = 0; i < 128; i += 8) {
        uint64_t v = rnd();
        memcpy(mem + i, &v, 8);
    }
    if (modrm >= 0xC0)
        return;
    if (op == 0xD8 || (op == 0xD9 && reg == 0)) {
        uint32_t v = rnd_f32();
        memcpy(mem, &v, 4);
    } else if (op == 0xDC || (op == 0xDD && reg == 0)) {
        uint64_t v = rnd_f64();
        memcpy(mem, &v, 8);
    } else if (op == 0xDA || op == 0xDE || (op == 0xDB && reg == 0) || (op == 0xDF && (reg == 0 || reg == 5))) {
        uint64_t v = rnd_int();
        memcpy(mem, &v, 8);
    } else if (op == 0xDB && reg == 5) {
        x87_f80_store(mem, rnd_f80());
    } else if (op == 0xDF && reg == 4) {
        if (rn(4)) {
            for (int i = 0; i < 9; i++)
                mem[i] = (uint8_t)((rn(10) << 4) | rn(10));
            if (rn(4) == 0)
                memset(mem + rn(9), 0, 9 - rn(9));
        }
    } else if ((op == 0xD9 && reg == 4) || (op == 0xDD && reg == 4)) {
        gen_state(mem, 1);
        if (rn(2)) { mem[2] = (uint8_t)rnd(); mem[6] = (uint8_t)rnd(); mem[10] = (uint8_t)rnd(); }
    } else if (op == 0xD9 && reg == 5) {
        uint8_t st[108];
        gen_state(st, 1);
        memcpy(mem, st, 2);
    }
}

static void dump_state(const char *name, const uint8_t *st)
{
    unsigned cw = st[0] | (st[1] << 8), sw = st[4] | (st[5] << 8), tw = st[8] | (st[9] << 8);
    printf("  %-5s cw=%04x sw=%04x (top=%u %s%s%s%s es=%u sf=%u ex=%02x) tw=%04x\n", name, cw, sw, (sw >> 11) & 7,
           sw & 0x4000 ? "C3 " : "", sw & 0x400 ? "C2 " : "", sw & 0x200 ? "C1 " : "", sw & 0x100 ? "C0 " : "",
           (sw >> 7) & 1, (sw >> 6) & 1, sw & 0x3F, tw);
    printf("       ");
    for (int i = 0; i < 8; i++) {
        x87_f80 v = x87_f80_load(st + 28 + 10 * i);
        printf(" %04x:%016llx", v.sexp, (unsigned long long)v.mant);
        if (i == 3) printf("\n       ");
    }
    printf("\n");
}

static void dump_mem(const char *name, const uint8_t *m, size_t n)
{
    printf("  %-5s mem:", name);
    for (size_t i = 0; i < n && i < 32; i++)
        printf(" %02x", m[i]);
    printf("\n");
}

/* FXSAVE64 / FXRSTOR64 against the host. */
static unsigned long test_fxsave(long count)
{
    unsigned long bad = 0;
    static uint8_t img[108], out_h[108], out_e[108];
    static uint8_t area_h[512] __attribute__((aligned(16))), area_e[512];
    for (long n = 0; n < count; n++) {
        x87_state s;
        x87_insn in;
        uint8_t tmp[108];
        gen_state(img, 1);
        /* host: frstor; fxsave64; (fninit); fxrstor64; fnsave */
        __asm__ volatile("frstor %0\n fxsave64 %1\n fninit\n fxrstor64 %1\n fnsave %2"
                         : : "m"(*(uint8_t(*)[108])img), "m"(*(uint8_t(*)[512])area_h), "m"(*(uint8_t(*)[108])out_h) : "memory");
        x87_init(&s);
        memcpy(tmp, img, 108);
        memset(&in, 0, sizeof in);
        in.op = 0xDD; in.modrm = 0x26; in.mem = tmp;
        x87_exec(&s, &in);
        memcpy(area_e, area_h, 512);
        memset(area_e, 0xAA, 6); memset(area_e + 32, 0xAA, 128);
        x87_fxsave(&s, area_e, 1);
        x87_init(&s);
        x87_fxrstor(&s, area_e, 1);
        in.modrm = 0x36; in.mem = out_e;
        x87_exec(&s, &in);
        memset(out_h + 12, 0, 16); memset(out_e + 12, 0, 16);
        if (memcmp(area_h, area_e, 6) || memcmp(area_h + 32, area_e + 32, 128) || memcmp(out_h, out_e, 108)) {
            if (!bad) { printf("FXSAVE mismatch\n"); dump_state("in", img); dump_state("host", out_h); dump_state("emu", out_e); }
            bad++;
        }
    }
    return bad;
}

/* The results are specific to the CPU model: say which one this is. */
static void print_host_cpu(void)
{
    unsigned a, b, c, d, r[12];
    char brand[49];
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    {
        unsigned fam = (a >> 8) & 15, mod = (a >> 4) & 15;
        if (fam == 6 || fam == 15) mod |= ((a >> 16) & 15) << 4;
        if (fam == 15) fam += (a >> 20) & 255;
        for (unsigned i = 0; i < 3; i++)
            __asm__("cpuid" : "=a"(r[4 * i]), "=b"(r[4 * i + 1]), "=c"(r[4 * i + 2]), "=d"(r[4 * i + 3])
                            : "a"(0x80000002u + i), "c"(0));
        memcpy(brand, r, 48);
        brand[48] = 0;
        printf("host CPU: %s (family %u model %u stepping %u)\n", brand, fam, mod, a & 15);
    }
    (void)b; (void)c; (void)d;
}

int main(int argc, char **argv)
{
    long count = 1000000, maxprint = 10, printed = 0;
    int only_op = -1, only_modrm = -1, strict_trans = 0, only_trans = 0, no_trans = 0, accuracy = 0;
    static unsigned long fails[8][256], runs[8][256], softfails[8][256];
    unsigned long total_fail = 0, total_soft = 0;
    uint64_t max_ulp = 0;
    struct sigaction sa;
    FILE *rec = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) rng_s ^= strtoull(argv[++i], 0, 0) * 0x9E3779B97F4A7C15ull + 1;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) count = strtol(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) only_op = (int)strtol(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) only_modrm = (int)strtol(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) maxprint = strtol(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-t")) strict_trans = 1;
        else if (!strcmp(argv[i], "-T")) only_trans = 1;
        else if (!strcmp(argv[i], "-N")) no_trans = 1;
        else if (!strcmp(argv[i], "-G") && i + 1 < argc) {
            rec = fopen(argv[++i], "wb");
            if (!rec) { perror("fopen"); return 2; }
        }
        else if (!strcmp(argv[i], "-A")) accuracy = only_trans = 1;
        else { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
    }
    print_host_cpu();
    stub = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) { perror("mmap"); return 2; }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGFPE, &sa, 0);
    sigaction(SIGILL, &sa, 0);

    if (!accuracy && only_op < 0) {
        unsigned long fx = test_fxsave(count / 100 + 100);
        printf("FXSAVE/FXRSTOR: %lu mismatches\n", fx);
        total_fail += fx;
    }

    for (long n = 0; n < count; n++) {
        static tcase t;
        static tresult h, e;
        unsigned op = 0xD8 + rn(8), modrm, trans;
        int acc, soft = 0, bad = 0, pfx66 = 0, is_env;
        size_t msz;

        if (only_trans) {
            static const uint8_t tm[8] = { 0xF0, 0xF1, 0xF2, 0xF3, 0xF9, 0xFB, 0xFE, 0xFF };
            op = 0xD9; modrm = tm[rn(8)];
        } else {
            modrm = rn(3) ? 0xC0 + rn(64) : (rn(8) << 3);
        }
        if (only_op >= 0) op = (unsigned)only_op;
        if (only_modrm >= 0) modrm = (unsigned)only_modrm;
        trans = is_trans(op, modrm);
        if (trans && no_trans) continue;

        if (accuracy)
            gen_accuracy_state(modrm, t.in_state);
        else
            gen_state(t.in_state, rn(8) == 0);
        gen_mem(op, modrm, t.mem);
        t.eflags = 0x202 | (rnd() & 0x8D5);
        /* environment instructions: also exercise the 16-bit layout */
        is_env = modrm < 0xC0 && (op == 0xD9 || op == 0xDD) && ((modrm & 0x38) == 0x20 || (modrm & 0x38) == 0x30);
        if (is_env && rn(2)) {
            pfx66 = 1;
            if ((modrm & 0x38) == 0x20) {
                /* convert the generated 32-bit image into the 16-bit layout */
                uint8_t img[108];
                memcpy(img, t.mem, 108);
                memcpy(t.mem, img, 2); memcpy(t.mem + 2, img + 4, 2); memcpy(t.mem + 4, img + 8, 2);
                memset(t.mem + 6, 0, 8);
                memcpy(t.mem + 14, img + 28, 80);
            }
        }
        msz = x87_mem_operand((uint8_t)op, (uint8_t)modrm, pfx66 ? X87_ENV_PM16 : X87_ENV_PM32, &acc);

        run_host(op, modrm, pfx66, &t, &h);
        run_emu(op, modrm, pfx66, &t, &e);

        if (rec) {
            /* record: header, input state, input operand, host state, host operand */
            uint8_t hd[12] = { (uint8_t)op, (uint8_t)modrm, (uint8_t)pfx66, (uint8_t)h.result,
                               (uint8_t)t.eflags, (uint8_t)(t.eflags >> 8), (uint8_t)h.eflags, (uint8_t)(h.eflags >> 8),
                               (uint8_t)h.ax, (uint8_t)(h.ax >> 8), (uint8_t)msz, 0 };
            fwrite(hd, 1, 12, rec);
            fwrite(t.in_state, 1, 108, rec);
            fwrite(t.mem, 1, msz, rec);
            fwrite(h.state, 1, 108, rec);
            fwrite(h.mem, 1, msz, rec);
        }
        /* ignore FIP/FCS/FOP/FDP/FDS */
        memset(h.state + 12, 0, 16); memset(e.state + 12, 0, 16);
        if (is_env && (modrm & 0x38) == 0x30) {
            if (pfx66) { memset(h.mem + 6, 0, 8); memset(e.mem + 6, 0, 8); }
            else { memset(h.mem + 12, 0, 16); memset(e.mem + 12, 0, 16); }
        }
        runs[op & 7][modrm]++;
        if (h.result != e.result || h.ax != e.ax || h.eflags != e.eflags ||
            memcmp(h.mem, e.mem, sizeof h.mem)) {
            bad = 1;
        } else if (memcmp(h.state, e.state, 108)) {
            bad = 1;
            if (trans && !strict_trans && !memcmp(h.state, e.state, 4) && !memcmp(h.state + 6, e.state + 6, 6)) {
                /* tolerate value differences (and C1/PE) for transcendentals */
                unsigned hs = h.state[4] | (h.state[5] << 8), es = e.state[4] | (e.state[5] << 8);
                if (((hs ^ es) & ~0x0200u) == 0) { soft = 1; bad = 0; }
            }
        }
        if (soft) {
            softfails[op & 7][modrm]++; total_soft++;
            /* track the largest distance in units in the last place */
            for (int k = 0; k < 8; k++) {
                x87_f80 a = x87_f80_load(h.state + 28 + 10 * k), b = x87_f80_load(e.state + 28 + 10 * k);
                uint64_t d;
                if (a.sexp == b.sexp)
                    d = a.mant > b.mant ? a.mant - b.mant : b.mant - a.mant;
                else if ((a.sexp ^ b.sexp) & 0x8000)
                    d = ~0ull;
                else {
                    x87_f80 lo = a.sexp < b.sexp ? a : b, hi = a.sexp < b.sexp ? b : a;
                    d = (hi.sexp - lo.sexp == 1 && hi.mant == 0x8000000000000000ull) ? (0 - lo.mant) : ~0ull;
                }
                if (d > max_ulp) max_ulp = d;
            }
        }
        if (bad) {
            fails[op & 7][modrm]++;
            total_fail++;
            if (printed < maxprint) {
                printed++;
                printf("MISMATCH #%ld: %s%02X %02X  eflags_in=%03llx\n", n, pfx66 ? "66 " : "", op, modrm, (unsigned long long)(t.eflags & 0x8D5));
                dump_state("in", t.in_state);
                if (msz) dump_mem("in", t.mem, msz);
                printf("  host: result=%d ax=%04llx eflags=%03llx\n", h.result, (unsigned long long)h.ax, (unsigned long long)h.eflags);
                dump_state("host", h.state);
                if (msz) dump_mem("host", h.mem, msz);
                printf("  emu : result=%d ax=%04llx eflags=%03llx\n", e.result, (unsigned long long)e.ax, (unsigned long long)e.eflags);
                dump_state("emu", e.state);
                if (msz) dump_mem("emu", e.mem, msz);
            }
        }
    }
    if (accuracy) {
        static const struct { int m; const char *name; } tn[8] = {
            { 0xFE, "FSIN" }, { 0xFF, "FCOS" }, { 0xFB, "FSINCOS" }, { 0xF2, "FPTAN" },
            { 0xF3, "FPATAN" }, { 0xF0, "F2XM1" }, { 0xF1, "FYL2X" }, { 0xF9, "FYL2XP1" } };
        printf("bit-exact rate against this host (values, flags and C1):\n");
        for (int i = 0; i < 8; i++) {
            unsigned long r = runs[1][tn[i].m], f = fails[1][tn[i].m] + softfails[1][tn[i].m];
            printf("  %-8s %7.3f%%  (%lu of %lu differ; %lu in more than the value)\n", tn[i].name,
                   r ? 100.0 * (double)(r - f) / (double)r : 0.0, f, r, fails[1][tn[i].m]);
        }
        printf("largest value difference: %llu ulp\n", (unsigned long long)max_ulp);
        return 0;
    }
    if (total_fail || total_soft) {
        printf("per-opcode failures (hard/soft/runs):\n");
        for (int o = 0; o < 8; o++)
            for (int m = 0; m < 256; m++)
                if (fails[o][m] || softfails[o][m])
                    printf("  %02X %02X: %lu / %lu / %lu\n", 0xD8 + o, m, fails[o][m], softfails[o][m], runs[o][m]);
    }
    if (rec)
        fclose(rec);
    printf("%ld cases, %lu mismatches, %lu transcendental value-only differences (largest: %llu ulp)\n", count, total_fail,
           total_soft, (unsigned long long)max_ulp);
    return total_fail != 0;
}
