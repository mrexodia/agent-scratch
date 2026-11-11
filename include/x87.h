/* SPDX-License-Identifier: MIT
 *
 * x87 - a freestanding, hardware-faithful x87 FPU emulation library.
 *
 * The library models the complete programmer-visible state of a P6-and-later
 * x87 FPU (data registers, control/status/tag words, last instruction and
 * data pointers, opcode) and executes instructions given as the two-byte
 * (ESC opcode, ModRM) pair plus a pointer to the memory operand.
 *
 * It does not allocate, has no global state and uses no host floating point.
 */
#ifndef X87_H
#define X87_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 80-bit double extended value: 64-bit significand with explicit integer
 * bit, and sign + 15-bit biased exponent. */
typedef struct x87_f80 {
    uint64_t mant;
    uint16_t sexp;
} x87_f80;

typedef struct x87_state {
    x87_f80  r[8];   /* physical data registers R0..R7 (not ST-relative) */
    uint16_t cw;     /* control word */
    uint16_t sw;     /* status word (TOP in bits 11..13) */
    uint8_t  valid;  /* bit i set: physical register i is not empty */
    uint16_t fop;    /* last opcode (11 bits) */
    uint16_t fcs;    /* last instruction pointer selector */
    uint16_t fds;    /* last data pointer selector */
    uint64_t fip;    /* last instruction pointer offset */
    uint64_t fdp;    /* last data pointer offset */
} x87_state;

/* Status word bits */
#define X87_SW_IE  0x0001u
#define X87_SW_DE  0x0002u
#define X87_SW_ZE  0x0004u
#define X87_SW_OE  0x0008u
#define X87_SW_UE  0x0010u
#define X87_SW_PE  0x0020u
#define X87_SW_SF  0x0040u
#define X87_SW_ES  0x0080u
#define X87_SW_C0  0x0100u
#define X87_SW_C1  0x0200u
#define X87_SW_C2  0x0400u
#define X87_SW_C3  0x4000u
#define X87_SW_B   0x8000u

/* Result bits of x87_exec() */
#define X87_R_OK        0x00
#define X87_R_MF        0x01 /* an unmasked exception was pending: the
                                instruction was NOT executed; deliver #MF (or
                                assert FERR#) and re-execute afterwards */
#define X87_R_UD        0x02 /* not a valid x87 encoding: nothing executed */
#define X87_R_MEM_WRITE 0x04 /* the memory operand was written */
#define X87_R_AX        0x08 /* insn.ax was written (FNSTSW AX) */
#define X87_R_EFLAGS    0x10 /* insn.eflags was written (F[U]COMI[P]) */

/* Memory image formats for FLDENV/FNSTENV/FRSTOR/FNSAVE */
#define X87_ENV_PM32 0 /* 32-bit protected mode (28/108 bytes); also 64-bit mode */
#define X87_ENV_PM16 1 /* 16-bit protected mode (14/94 bytes) */
#define X87_ENV_RM32 2 /* 32-bit real/v86 mode */
#define X87_ENV_RM16 3 /* 16-bit real/v86 mode */

typedef struct x87_insn {
    uint8_t  op;      /* first opcode byte, 0xD8..0xDF */
    uint8_t  modrm;   /* second byte */
    uint8_t  env_fmt; /* X87_ENV_* (only used by the environment insns) */
    void    *mem;     /* memory operand (ModRM < 0xC0); see x87_mem_operand() */
    uint64_t ip;      /* address of the instruction, recorded as FIP */
    uint64_t dp;      /* effective address of the operand, recorded as FDP */
    uint16_t cs, ds;  /* selectors recorded with FIP / FDP */
    uint32_t eflags;  /* in: FCMOVcc; out: F[U]COMI[P] (CF, PF, ZF; OF/SF/AF cleared) */
    uint16_t ax;      /* out: FNSTSW AX */
} x87_insn;

/* Memory operand description. */
#define X87_MEM_NONE  0
#define X87_MEM_READ  1
#define X87_MEM_WRITE 2
/* Returns the operand size in bytes (0 if the encoding has no memory operand
 * or is invalid) and stores X87_MEM_* in *access. */
size_t x87_mem_operand(uint8_t op, uint8_t modrm, int env_fmt, int *access);

/* Power-on / FNINIT state. */
void x87_init(x87_state *s);

/* Execute one instruction. Returns a mask of X87_R_*. */
int x87_exec(x87_state *s, x87_insn *insn);

/* FWAIT / WAIT (0x9B): returns X87_R_MF if an unmasked exception is pending. */
int x87_fwait(const x87_state *s);

/* Non-zero if an unmasked exception is pending (ES set); this is the level
 * of the FERR# pin (active). */
int x87_pending(const x87_state *s);

/* Full (two bits per register) tag word as stored by FNSAVE/FNSTENV. */
uint16_t x87_tag_word(const x87_state *s);

/* FXSAVE/FXRSTOR x87 portion: bytes 0..23 and 32..159 of the 512-byte area
 * (MXCSR and the XMM registers are left untouched / ignored).  If rex_w is
 * set the 64-bit FIP/FDP layout is used. */
void x87_fxsave(const x87_state *s, uint8_t *area, int rex_w);
void x87_fxrstor(x87_state *s, const uint8_t *area, int rex_w);

/* MMX support: register access aliasing as done by MMX instructions. */
uint64_t x87_mmx_read(const x87_state *s, int reg);
void x87_mmx_write(x87_state *s, int reg, uint64_t value);
void x87_emms(x87_state *s);

/* Convenience accessors. */
static inline unsigned x87_top(const x87_state *s) { return (s->sw >> 11) & 7; }
static inline x87_f80 *x87_st(x87_state *s, unsigned i) { return &s->r[(x87_top(s) + i) & 7]; }
static inline int x87_st_empty(const x87_state *s, unsigned i)
{
    return !((s->valid >> ((x87_top(s) + i) & 7)) & 1);
}

/* Little-endian 10-byte memory image <-> x87_f80. */
x87_f80 x87_f80_load(const void *p);
void x87_f80_store(void *p, x87_f80 v);

#ifdef __cplusplus
}
#endif
#endif /* X87_H */
