/* SPDX-License-Identifier: MIT
 *
 * x87trace: run any x86-64 Linux program under ptrace, single-stepping it,
 * and check every x87 instruction it executes on the real CPU against the
 * emulator - same input state, same memory operand, compare everything.
 *
 * This turns existing test suites and real programs (QEMU's tcg tests,
 * libm, code built with -mfpmath=387, ...) into hardware-verified test
 * cases without needing their expected values: the CPU is the oracle.
 *
 * usage: x87trace [-G vectors.bin] [-v maxprint] [-q] [-s sym[,sym...]] [-n count] program [args...]
 *   -G  record each distinct case in the tests/replay format
 *   -q  discard the program's stdout
 *   -s  additional functions to run at full speed instead of stepping
 *   -n  stop after this many x87 instructions
 *
 * Single-stepping is slow, so for statically linked non-PIE programs (build
 * with -static -no-pie) the tracer reads the symbol table, runs at full speed
 * up to main(), and does not step through the stdio functions.
 */
#if !defined(__x86_64__) || !defined(__linux__)
#error "x87trace needs x86-64 Linux"
#endif
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#include "x87.h"

static pid_t child;

static int peek(uint64_t addr, uint8_t *buf, size_t n)
{
    struct iovec l = { buf, n }, r = { (void *)addr, n };
    if (process_vm_readv(child, &l, 1, &r, 1, 0) == (ssize_t)n)
        return 0;
    /* fall back to word-wise reads (also handles reads ending at a page edge) */
    for (size_t i = 0; i < n; i += 8) {
        long w;
        errno = 0;
        w = ptrace(PTRACE_PEEKDATA, child, (void *)(addr + i), 0);
        if (errno)
            return -1;
        memcpy(buf + i, &w, n - i < 8 ? n - i : 8);
    }
    return 0;
}

static void poke8(uint64_t addr, uint8_t v)
{
    long w = ptrace(PTRACE_PEEKTEXT, child, (void *)addr, 0);
    w = (long)(((unsigned long)w & ~0xFFul) | v);
    ptrace(PTRACE_POKETEXT, child, (void *)addr, (void *)w);
}

/* Run at full speed until the instruction at addr is reached.  Returns 0 if
 * the program ended first. */
static int run_to(uint64_t addr)
{
    uint8_t orig;
    int status, sig = 0;
    struct user_regs_struct r;
    if (peek(addr, &orig, 1) < 0)
        return 1; /* cannot plant a breakpoint: keep stepping */
    poke8(addr, 0xCC);
    for (;;) {
        if (ptrace(PTRACE_CONT, child, 0, sig) < 0)
            return 0;
        if (waitpid(child, &status, 0) < 0 || WIFEXITED(status) || WIFSIGNALED(status))
            return 0;
        ptrace(PTRACE_GETREGS, child, 0, &r);
        if (WSTOPSIG(status) == SIGTRAP && r.rip == addr + 1)
            break;
        sig = WSTOPSIG(status) == SIGTRAP ? 0 : WSTOPSIG(status);
    }
    poke8(addr, orig);
    r.rip = addr;
    ptrace(PTRACE_SETREGS, child, 0, &r);
    return 1;
}

/* ---- symbols of a static, non-PIE executable ---- */
#define MAX_SKIP 64
static uint64_t skip_addr[MAX_SKIP], main_addr;
static int nskip;

static void load_symbols(const char *path, const char *extra)
{
    static const char *const defaults[] = {
        "printf", "puts", "putchar", "fprintf", "vprintf", "vfprintf", "fflush", "fputs", "fwrite", "sprintf",
        "snprintf", "__printf_chk", "__fprintf_chk", "__sprintf_chk", "__snprintf_chk", "exit", "abort", "malloc",
        "free", "calloc", 0 };
    FILE *f = fopen(path, "rb");
    Elf64_Ehdr eh;
    Elf64_Shdr *sh = 0;
    if (!f)
        return;
    if (fread(&eh, sizeof eh, 1, f) != 1 || memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_type != ET_EXEC || !eh.e_shnum)
        goto out;
    sh = malloc(sizeof *sh * eh.e_shnum);
    if (!sh || fseek(f, (long)eh.e_shoff, SEEK_SET) || fread(sh, sizeof *sh, eh.e_shnum, f) != eh.e_shnum)
        goto out;
    for (unsigned i = 0; i < eh.e_shnum; i++) {
        Elf64_Sym *sym;
        char *str;
        size_t n, strsz;
        if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= eh.e_shnum)
            continue;
        n = sh[i].sh_size / sizeof *sym;
        strsz = sh[sh[i].sh_link].sh_size;
        sym = malloc(sh[i].sh_size);
        str = malloc(strsz + 1);
        if (sym && str && !fseek(f, (long)sh[i].sh_offset, SEEK_SET) && fread(sym, sizeof *sym, n, f) == n &&
            !fseek(f, (long)sh[sh[i].sh_link].sh_offset, SEEK_SET) && fread(str, 1, strsz, f) == strsz) {
            str[strsz] = 0;
            for (size_t k = 0; k < n; k++) {
                const char *name = sym[k].st_name < strsz ? str + sym[k].st_name : "";
                int want = 0;
                if (ELF64_ST_TYPE(sym[k].st_info) != STT_FUNC || !sym[k].st_value)
                    continue;
                if (!strcmp(name, "main"))
                    main_addr = sym[k].st_value;
                for (int d = 0; defaults[d]; d++)
                    if (!strcmp(name, defaults[d])) want = 1;
                if (extra) {
                    size_t len = strlen(name);
                    for (const char *p = extra; *p;) {
                        const char *q = strchr(p, ',');
                        size_t l = q ? (size_t)(q - p) : strlen(p);
                        if (l == len && !memcmp(p, name, l)) want = 1;
                        p += l + (q != 0);
                    }
                }
                if (want && nskip < MAX_SKIP)
                    skip_addr[nskip++] = sym[k].st_value;
            }
        }
        free(sym);
        free(str);
    }
out:
    free(sh);
    fclose(f);
}

static uint64_t gpr(const struct user_regs_struct *r, unsigned i)
{
    switch (i) {
    case 0: return r->rax;  case 1: return r->rcx;  case 2: return r->rdx;  case 3: return r->rbx;
    case 4: return r->rsp;  case 5: return r->rbp;  case 6: return r->rsi;  case 7: return r->rdi;
    case 8: return r->r8;   case 9: return r->r9;   case 10: return r->r10; case 11: return r->r11;
    case 12: return r->r12; case 13: return r->r13; case 14: return r->r14; default: return r->r15;
    }
}

typedef struct {
    unsigned op, modrm, len;
    int      pfx66, has_mem;
    uint64_t ea;
} decoded;

/* Decode an x87 instruction at code[]; returns 0 if it is not one. */
static int decode(const uint8_t *code, const struct user_regs_struct *r, decoded *d)
{
    unsigned i = 0, rex = 0, mod, rm;
    int addr32 = 0, seg = 0;
    uint64_t ea = 0;

    d->pfx66 = 0;
    for (;; i++) {
        uint8_t b = code[i];
        if (b == 0x66) d->pfx66 = 1;
        else if (b == 0x67) addr32 = 1;
        else if (b == 0x64) seg = 1;
        else if (b == 0x65) seg = 2;
        else if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0xF2 || b == 0xF3) continue;
        else break;
        if (i > 8) return 0;
    }
    if ((code[i] & 0xF0) == 0x40)
        rex = code[i++];
    if (code[i] < 0xD8 || code[i] > 0xDF)
        return 0;
    d->op = code[i++];
    d->modrm = code[i++];
    mod = d->modrm >> 6;
    rm = d->modrm & 7;
    d->has_mem = mod != 3;
    if (mod != 3) {
        int32_t disp = 0;
        int riprel = 0;
        if (rm == 4) {
            uint8_t sib = code[i++];
            unsigned base = (sib & 7) | ((rex & 1) << 3), idx = ((sib >> 3) & 7) | ((rex & 2) << 2);
            if (idx != 4)
                ea += gpr(r, idx) << (sib >> 6);
            if ((sib & 7) == 5 && mod == 0) {
                memcpy(&disp, code + i, 4); i += 4;
            } else {
                ea += gpr(r, base);
            }
        } else if (rm == 5 && mod == 0) {
            memcpy(&disp, code + i, 4); i += 4;
            riprel = 1;
        } else {
            ea += gpr(r, rm | ((rex & 1) << 3));
        }
        if (mod == 1) {
            disp = (int8_t)code[i++];
        } else if (mod == 2) {
            memcpy(&disp, code + i, 4); i += 4;
        }
        ea += (uint64_t)(int64_t)disp;
        if (riprel)
            ea += r->rip + i;
        if (addr32)
            ea &= 0xFFFFFFFFu;
        if (seg == 1) ea += r->fs_base;
        if (seg == 2) ea += r->gs_base;
    }
    d->ea = ea;
    d->len = i;
    return 1;
}

static int is_trans(unsigned op, unsigned modrm)
{
    if (op != 0xD9) return 0;
    return modrm == 0xF0 || modrm == 0xF1 || modrm == 0xF2 || modrm == 0xF3 || modrm == 0xF9 ||
           modrm == 0xFB || modrm == 0xFE || modrm == 0xFF;
}

/* FXSAVE image -> the FNSAVE image the replay format uses */
static void to_fnsave(const uint8_t *fx, uint8_t *img)
{
    x87_state s;
    x87_insn in;
    x87_init(&s);
    x87_fxrstor(&s, fx, 1);
    memset(&in, 0, sizeof in);
    in.op = 0xDD; in.modrm = 0x36; in.mem = img; in.env_fmt = X87_ENV_PM32;
    x87_exec(&s, &in);
    memset(img + 12, 0, 16);
}

/* small open-addressing set of 64-bit hashes for de-duplication */
#define SET_SIZE (1u << 22)
static uint64_t *seen;
static int seen_add(uint64_t h)
{
    uint32_t i = (uint32_t)(h >> 20) & (SET_SIZE - 1);
    if (!h) h = 1;
    while (seen[i]) {
        if (seen[i] == h) return 0;
        i = (i + 1) & (SET_SIZE - 1);
    }
    seen[i] = h;
    return 1;
}
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--) h = (h ^ *b++) * 0x100000001B3ull;
    return h;
}

int main(int argc, char **argv)
{
    static unsigned long runs[8][256], bad_n[8][256], soft_n[8][256];
    unsigned long total = 0, uniq = 0, hard = 0, soft = 0, steps = 0;
    long maxprint = 10;
    unsigned long limit = 0;
    int a = 1, quiet = 0, status;
    const char *extra_skip = 0;
    FILE *rec = 0;

    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-G") && a + 1 < argc) {
            rec = fopen(argv[++a], "ab");
            if (!rec) { perror("fopen"); return 2; }
        } else if (!strcmp(argv[a], "-v") && a + 1 < argc) maxprint = strtol(argv[++a], 0, 0);
        else if (!strcmp(argv[a], "-q")) quiet = 1;
        else if (!strcmp(argv[a], "-n") && a + 1 < argc) limit = strtoul(argv[++a], 0, 0);
        else if (!strcmp(argv[a], "-s") && a + 1 < argc) extra_skip = argv[++a];
        else break;
    }
    if (a >= argc) {
        fprintf(stderr, "usage: x87trace [-G vectors.bin] [-v maxprint] [-q] [-s sym,...] [-n count] program [args...]\n");
        return 2;
    }
    seen = calloc(SET_SIZE, sizeof *seen);
    if (!seen) return 2;

    child = fork();
    if (child == 0) {
        if (quiet) {
            if (!freopen("/dev/null", "w", stdout)) _exit(126);
        }
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        execvp(argv[a], argv + a);
        perror("execvp");
        _exit(127);
    }
    if (waitpid(child, &status, 0) < 0 || !WIFSTOPPED(status)) {
        fprintf(stderr, "x87trace: could not start %s\n", argv[a]);
        return 2;
    }
    ptrace(PTRACE_SETOPTIONS, child, 0, PTRACE_O_EXITKILL);
    load_symbols(argv[a], extra_skip);
    if (main_addr && !run_to(main_addr))
        main_addr = 0; /* program ended */

    for (;;) {
        struct user_regs_struct r0, r1;
        struct user_fpregs_struct f0, f1;
        uint8_t code[24], mem0[128], mem1[128], emem[128], efx[512];
        decoded d = { 0, 0, 0, 0, 0, 0 };
        int sig = 0, is_x87 = 0, acc = 0;
        size_t msz = 0;

        if (limit && total >= limit) {
            kill(child, SIGKILL);
            break;
        }
        if (ptrace(PTRACE_GETREGS, child, 0, &r0) < 0)
            break;
        /* entering a function we do not want to step through? */
        {
            int skip = 0;
            for (int k = 0; k < nskip; k++)
                if (skip_addr[k] == r0.rip) skip = 1;
            if (skip) {
                uint64_t ret;
                if (peek(r0.rsp, (uint8_t *)&ret, 8) == 0) {
                    if (!run_to(ret))
                        break;
                    continue;
                }
            }
        }
        memset(code, 0x90, sizeof code);
        if ((peek(r0.rip, code, sizeof code) == 0 || peek(r0.rip, code, 8) == 0) && decode(code, &r0, &d)) {
            int fmt = d.pfx66 ? X87_ENV_PM16 : X87_ENV_PM32;
            msz = x87_mem_operand((uint8_t)d.op, (uint8_t)d.modrm, fmt, &acc);
            is_x87 = 1;
            ptrace(PTRACE_GETFPREGS, child, 0, &f0);
            memset(mem0, 0, sizeof mem0);
            if (msz && peek(d.ea, mem0, msz) < 0)
                is_x87 = 0; /* faulting operand: let the program deal with it */
        }

        if (ptrace(PTRACE_SINGLESTEP, child, 0, 0) < 0)
            break;
        if (waitpid(child, &status, 0) < 0 || WIFEXITED(status) || WIFSIGNALED(status))
            break;
        steps++;
        if (WSTOPSIG(status) != SIGTRAP)
            sig = WSTOPSIG(status);

        if (is_x87 && sig != SIGSEGV && sig != SIGBUS) {
            x87_state s;
            x87_insn in;
            int res, eres, hres, bad = 0, softdiff = 0;
            unsigned o = d.op & 7, m = d.modrm;
            uint32_t e_ef, h_ef;
            uint16_t e_ax, h_ax;
            const uint8_t *hfx = (const uint8_t *)&f1;

            ptrace(PTRACE_GETREGS, child, 0, &r1);
            ptrace(PTRACE_GETFPREGS, child, 0, &f1);
            memcpy(mem1, mem0, sizeof mem1);
            if (msz)
                peek(d.ea, mem1, msz);
            hres = sig == SIGFPE ? 1 : sig == SIGILL ? 2 : 0;

            x87_init(&s);
            x87_fxrstor(&s, (const uint8_t *)&f0, 1);
            memcpy(emem, mem0, sizeof emem);
            memset(&in, 0, sizeof in);
            in.op = (uint8_t)d.op; in.modrm = (uint8_t)d.modrm; in.mem = emem;
            in.env_fmt = (uint8_t)(d.pfx66 ? X87_ENV_PM16 : X87_ENV_PM32);
            in.eflags = (uint32_t)r0.eflags;
            in.ax = (uint16_t)r0.rax;
            res = x87_exec(&s, &in);
            eres = (res & X87_R_MF) ? 1 : (res & X87_R_UD) ? 2 : 0;
            e_ef = in.eflags & 0x8D5; h_ef = (uint32_t)r1.eflags & 0x8D5;
            e_ax = in.ax; h_ax = (uint16_t)r1.rax;
            memset(efx, 0, sizeof efx);
            x87_fxsave(&s, efx, 1);

            /* environment stores contain host code/data addresses */
            if (m < 0xC0 && (d.op == 0xD9 || d.op == 0xDD) && (m & 0x38) == 0x30) {
                if (d.pfx66) { memset(mem1 + 6, 0, 8); memset(emem + 6, 0, 8); }
                else { memset(mem1 + 12, 0, 16); memset(emem + 12, 0, 16); }
            }
            if (eres != hres || e_ef != h_ef || e_ax != h_ax || memcmp(emem, mem1, sizeof emem))
                bad = 1;
            if (memcmp(efx, hfx, 5))
                bad = 1; /* control word, status word, abridged tags */
            for (int k = 0; k < 8 && !bad; k++)
                if (memcmp(efx + 32 + 16 * k, hfx + 32 + 16 * k, 10))
                    softdiff = 1;
            if (softdiff && !bad && !is_trans(d.op, m))
                bad = 1;
            if (bad && is_trans(d.op, m) && eres == hres && e_ef == h_ef && e_ax == h_ax &&
                !memcmp(efx, hfx, 2) && efx[4] == hfx[4] &&
                (((efx[2] | (efx[3] << 8)) ^ (hfx[2] | (hfx[3] << 8))) & ~0x0200) == 0) {
                bad = 0; /* only C1 and the value differ */
                softdiff = 1;
            }

            total++;
            runs[o][m]++;
            if (bad) { hard++; bad_n[o][m]++; }
            else if (softdiff) { soft++; soft_n[o][m]++; }

            if (bad && (long)hard <= maxprint) {
                printf("MISMATCH at rip=%llx: %s%02X %02X ea=%llx\n", (unsigned long long)r0.rip, d.pfx66 ? "66 " : "", d.op, m,
                       (unsigned long long)d.ea);
                printf("  in   cw=%04x sw=%04x ftw=%02x\n", f0.cwd, f0.swd, f0.ftw & 0xFF);
                printf("  host cw=%04x sw=%04x ftw=%02x result=%d eflags=%03x ax=%04x\n", f1.cwd, f1.swd, f1.ftw & 0xFF, hres, h_ef, h_ax);
                printf("  emu  cw=%04x sw=%04x ftw=%02x result=%d eflags=%03x ax=%04x\n", efx[0] | (efx[1] << 8),
                       efx[2] | (efx[3] << 8), efx[4], eres, e_ef, e_ax);
                for (int k = 0; k < 8; k++) {
                    x87_f80 vi = x87_f80_load((const uint8_t *)&f0 + 32 + 16 * k), vh = x87_f80_load(hfx + 32 + 16 * k),
                            ve = x87_f80_load(efx + 32 + 16 * k);
                    printf("  st%d in %04x:%016llx host %04x:%016llx emu %04x:%016llx%s\n", k, vi.sexp, (unsigned long long)vi.mant,
                           vh.sexp, (unsigned long long)vh.mant, ve.sexp, (unsigned long long)ve.mant,
                           (vh.sexp != ve.sexp || vh.mant != ve.mant) ? "  <--" : "");
                }
                if (msz) {
                    printf("  mem in/host/emu:");
                    for (size_t k = 0; k < msz && k < 16; k++) printf(" %02x/%02x/%02x", mem0[k], mem1[k], emem[k]);
                    printf("\n");
                }
            }

            if (msz <= 108) {
                uint8_t hd[12] = { (uint8_t)d.op, (uint8_t)m, (uint8_t)d.pfx66, (uint8_t)hres,
                                   (uint8_t)r0.eflags, (uint8_t)((r0.eflags >> 8) & 0x0F), (uint8_t)h_ef, (uint8_t)(h_ef >> 8),
                                   0, 0, (uint8_t)msz, 0 };
                uint8_t img0[108], img1[108];
                uint64_t h;
                /* replay starts AX at 0x5A5A and expects it back unless written */
                uint16_t rec_ax = (res & X87_R_AX) ? h_ax : 0x5A5A;
                hd[4] &= 0xD5;
                hd[8] = (uint8_t)rec_ax; hd[9] = (uint8_t)(rec_ax >> 8);
                to_fnsave((const uint8_t *)&f0, img0);
                h = fnv(0xCBF29CE484222325ull, hd, 6);
                h = fnv(h, img0, 108);
                h = fnv(h, mem0, acc == X87_MEM_READ ? msz : 0);
                if (seen_add(h)) {
                    uniq++;
                    if (rec && !(res & X87_R_AX && 0)) {
                        to_fnsave(hfx, img1);
                        fwrite(hd, 1, 12, rec);
                        fwrite(img0, 1, 108, rec);
                        fwrite(mem0, 1, msz, rec);
                        fwrite(img1, 1, 108, rec);
                        fwrite(mem1, 1, msz, rec);
                    }
                }
            }
        }
        if (sig) {
            /* deliver the signal the instruction raised */
            if (ptrace(PTRACE_CONT, child, 0, sig) < 0)
                break;
            if (waitpid(child, &status, 0) < 0 || WIFEXITED(status) || WIFSIGNALED(status))
                break;
        }
    }
    if (rec)
        fclose(rec);

    if (hard || soft) {
        printf("per-opcode (mismatch / value-only / executed):\n");
        for (int o = 0; o < 8; o++)
            for (int m = 0; m < 256; m++)
                if (bad_n[o][m] || soft_n[o][m])
                    printf("  %02X %02X: %lu / %lu / %lu\n", 0xD8 + o, m, bad_n[o][m], soft_n[o][m], runs[o][m]);
    }
    {
        int kinds = 0;
        for (int o = 0; o < 8; o++)
            for (int m = 0; m < 256; m++)
                if (runs[o][m]) kinds++;
        printf("%s: %lu steps, %lu x87 instructions (%lu distinct cases, %d encodings): %lu mismatches, "
               "%lu transcendental value-only differences\n",
               argv[a], steps, total, uniq, kinds, hard, soft);
    }
    return hard != 0;
}
