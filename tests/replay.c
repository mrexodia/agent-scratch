/* SPDX-License-Identifier: MIT
 *
 * Replays test vectors recorded on real hardware by `hostdiff -G` and checks
 * that the emulator reproduces them.  Portable: runs on any host.
 *
 * Every instruction must match bit for bit, except that for the
 * transcendental instructions a difference confined to the result value
 * (and the C1 rounding indication) is counted separately and tolerated.
 *
 * usage: replay [-v] file...
 */
#include <stdio.h>
#include <string.h>
#include "x87.h"

static int is_trans(unsigned op, unsigned modrm)
{
    if (op != 0xD9) return 0;
    return modrm == 0xF0 || modrm == 0xF1 || modrm == 0xF2 || modrm == 0xF3 || modrm == 0xF9 ||
           modrm == 0xFB || modrm == 0xFE || modrm == 0xFF;
}

static void emu_insn(x87_state *s, unsigned op, unsigned modrm, void *mem, int fmt, x87_insn *in)
{
    memset(in, 0, sizeof *in);
    in->op = (uint8_t)op;
    in->modrm = (uint8_t)modrm;
    in->mem = mem;
    in->env_fmt = (uint8_t)fmt;
    (void)s;
}

int main(int argc, char **argv)
{
    unsigned long total = 0, hard = 0, soft = 0;
    int verbose = 0;

    for (int a = 1; a < argc; a++) {
        FILE *f;
        uint8_t hd[12];
        if (!strcmp(argv[a], "-v")) { verbose = 1; continue; }
        f = fopen(argv[a], "rb");
        if (!f) { perror(argv[a]); return 2; }
        while (fread(hd, 1, 12, f) == 12) {
            uint8_t in_state[108], h_state[108], e_state[108], tmp[108];
            uint8_t mem_in[128], h_mem[128], e_mem[160];
            unsigned op = hd[0], modrm = hd[1], msz = hd[10];
            int fmt = hd[2] ? X87_ENV_PM16 : X87_ENV_PM32, res, eres, bad = 0;
            unsigned ef_in = hd[4] | (hd[5] << 8), h_ef = hd[6] | (hd[7] << 8), h_ax = hd[8] | (hd[9] << 8);
            x87_state s;
            x87_insn in;

            if (msz > 128 || fread(in_state, 1, 108, f) != 108 || fread(mem_in, 1, msz, f) != msz ||
                fread(h_state, 1, 108, f) != 108 || fread(h_mem, 1, msz, f) != msz) {
                fprintf(stderr, "%s: truncated\n", argv[a]);
                return 2;
            }
            x87_init(&s);
            memcpy(tmp, in_state, 108);
            emu_insn(&s, 0xDD, 0x26, tmp, X87_ENV_PM32, &in);
            x87_exec(&s, &in);

            memset(e_mem, 0xA5, sizeof e_mem);
            memcpy(e_mem, mem_in, msz);
            emu_insn(&s, op, modrm, e_mem, fmt, &in);
            in.eflags = ef_in;
            in.ax = 0x5A5A;
            res = x87_exec(&s, &in);
            eres = (res & X87_R_MF) ? 1 : (res & X87_R_UD) ? 2 : 0;
            {
                unsigned e_ef = in.eflags & 0x8D5, e_ax = in.ax;
                emu_insn(&s, 0xDD, 0x36, e_state, X87_ENV_PM32, &in);
                x87_exec(&s, &in);
                /* FIP/FCS/FOP/FDP/FDS are host addresses: ignore */
                memset(h_state + 12, 0, 16); memset(e_state + 12, 0, 16);
                if (modrm < 0xC0 && (op == 0xD9 || op == 0xDD) && (modrm & 0x38) == 0x30) {
                    if (hd[2]) { memset(h_mem + 6, 0, 8); memset(e_mem + 6, 0, 8); }
                    else { memset(h_mem + 12, 0, 16); memset(e_mem + 12, 0, 16); }
                }
                for (unsigned i = msz; i < sizeof e_mem; i++)
                    if (e_mem[i] != 0xA5) bad = 1; /* wrote past the operand */
                if (eres != hd[3] || e_ef != h_ef || e_ax != h_ax || memcmp(e_mem, h_mem, msz))
                    bad = 1;
            }
            total++;
            if (!bad && memcmp(h_state, e_state, 108)) {
                unsigned hs = h_state[4] | (h_state[5] << 8), es = e_state[4] | (e_state[5] << 8);
                if (is_trans(op, modrm) && !memcmp(h_state, e_state, 4) && !memcmp(h_state + 6, e_state + 6, 6) &&
                    ((hs ^ es) & ~0x0200u) == 0)
                    soft++;
                else
                    bad = 1;
            }
            if (bad) {
                hard++;
                if (verbose || hard <= 10)
                    printf("%s: case %lu (%02X %02X) differs from hardware\n", argv[a], total, op, modrm);
            }
        }
        fclose(f);
    }
    printf("%lu vectors: %lu mismatches, %lu transcendental value-only differences\n", total, hard, soft);
    return hard != 0 || total == 0;
}
