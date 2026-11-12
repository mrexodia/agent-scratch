/* SPDX-License-Identifier: MIT
 *
 * Portable test of the state-level helpers that the host differential test
 * cannot reach: MMX aliasing and the environment layouts.
 */
#include <stdio.h>
#include <string.h>
#include "x87.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

static int exec(x87_state *s, unsigned op, unsigned modrm, void *mem, int fmt)
{
    x87_insn in;
    memset(&in, 0, sizeof in);
    in.op = (uint8_t)op; in.modrm = (uint8_t)modrm; in.mem = mem; in.env_fmt = (uint8_t)fmt;
    in.ip = 0x12345; in.cs = 0x0008; in.dp = 0x6789A; in.ds = 0x0010;
    return x87_exec(s, &in);
}

int main(void)
{
    x87_state s;
    uint8_t env[28], img[108];
    double d = 1.5;

    /* MMX: a read alone sets TOP = 0 and marks every register valid (measured
     * on hardware); a write also sets sign/exponent to all ones; EMMS empties */
    x87_init(&s);
    exec(&s, 0xDD, 0x00, &d, 0);                 /* fld m64: TOP = 7 */
    CHECK(x87_top(&s) == 7 && s.valid == 0x80);
    CHECK(x87_mmx_read(&s, 7) == 0xC000000000000000ull);
    CHECK(x87_top(&s) == 0 && s.valid == 0xFF);
    x87_mmx_write(&s, 3, 0x1234);
    CHECK(s.r[3].mant == 0x1234 && s.r[3].sexp == 0xFFFF && x87_tag_word(&s) != 0xFFFF);
    x87_emms(&s);
    CHECK(s.valid == 0 && x87_tag_word(&s) == 0xFFFF);

    /* FNSTENV layouts (Intel SDM Vol. 1 figures 8-9..8-12 / FP87::Store_Environment) */
    x87_init(&s);
    exec(&s, 0xDD, 0x00, &d, 0);                 /* records FIP/FDP/FOP: DD 00 -> FOP 0x500 */
    exec(&s, 0xD9, 0x30, env, X87_ENV_PM32);
    CHECK(env[0] == 0x7F && env[1] == 0x03 && env[2] == 0xFF && env[3] == 0xFF);   /* cw, reserved ones */
    CHECK(env[12] == 0x45 && env[13] == 0x23 && env[14] == 0x01 && env[15] == 0);  /* FIP */
    CHECK(env[16] == 0x08 && env[17] == 0 && env[18] == 0x00 && env[19] == 0x05);  /* FCS, FOP */
    CHECK(env[20] == 0x9A && env[21] == 0x78 && env[22] == 0x06);                  /* FDP */
    CHECK(env[24] == 0x10 && env[26] == 0xFF && env[27] == 0xFF);                  /* FDS, reserved ones */
    CHECK((s.cw & 0x3F) == 0x3F);                /* FNSTENV masks all exceptions */

    x87_init(&s);
    exec(&s, 0xDD, 0x00, &d, 0);
    exec(&s, 0xD9, 0x30, env, X87_ENV_RM16);     /* linear = (sel << 4) + offset */
    /* FIP linear = 0x80 + 0x12345 = 0x123C5; FDP linear = 0x100 + 0x6789A = 0x6799A */
    CHECK(env[6] == 0xC5 && env[7] == 0x23);
    CHECK(env[8] == 0x00 && env[9] == 0x15);     /* FIP[19:16]=1, 0, FOP=0x500 */
    CHECK(env[10] == 0x9A && env[11] == 0x79 && env[12] == 0x00 && env[13] == 0x60);

    x87_init(&s);
    exec(&s, 0xDD, 0x00, &d, 0);
    exec(&s, 0xD9, 0x30, env, X87_ENV_RM32);
    CHECK(env[12] == 0xC5 && env[13] == 0x23 && env[14] == 0xFF && env[15] == 0xFF);
    CHECK(env[16] == 0x00 && env[17] == 0x15 && env[18] == 0x00 && env[19] == 0x00); /* 0000 | FIP[31:16] | 0 | FOP */
    CHECK(env[20] == 0x9A && env[21] == 0x79 && env[24] == 0x00 && env[25] == 0x60);

    /* FLDENV: reserved control bits forced, ES/B recomputed, 16-bit protected
     * mode layout has no opcode field (FOP := 0) */
    memset(env, 0, sizeof env);
    env[0] = 0xBE; env[1] = 0xFF;                /* cw = 0xFFBE: IE and bit 6 clear, reserved bits set */
    env[2] = 0x01; env[3] = 0x00;                /* sw: IE set, ES clear */
    env[4] = 0xFF; env[5] = 0xFF;
    x87_init(&s);
    s.fop = 0x123;
    exec(&s, 0xD9, 0x20, env, X87_ENV_PM16);
    CHECK(s.cw == 0x1F7E);                       /* bit 6 reads 1; bits 7, 13-15 read 0 */
    CHECK((s.sw & 0x8080) == 0x8080);            /* unmasked IE pending: ES and B set */
    CHECK(s.fop == 0 && s.valid == 0);
    CHECK(exec(&s, 0xD9, 0xD0, 0, 0) == X87_R_MF);   /* FNOP is a waiting instruction */
    CHECK(exec(&s, 0xDB, 0xE2, 0, 0) == 0);          /* FNCLEX is not */
    CHECK((s.sw & 0x80FF) == 0 && exec(&s, 0xD9, 0xD0, 0, 0) == 0);

    /* FNSAVE stores the registers in stack order and re-initialises */
    x87_init(&s);
    exec(&s, 0xDD, 0x00, &d, 0);
    exec(&s, 0xDD, 0x30, img, X87_ENV_PM16);
    CHECK(img[14 + 7] == 0xC0 && img[14 + 8] == 0xFF && img[14 + 9] == 0x3F);   /* ST(0) = 1.5 */
    CHECK(s.cw == 0x037F && s.sw == 0 && s.valid == 0 && s.fip == 0 && s.fop == 0);

    printf("state_test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
