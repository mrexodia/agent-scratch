# agent-scratch

## x87 — a hardware-faithful x87 FPU emulation library (MIT)

A from-scratch, freestanding C11 implementation of the x87 FPU, written
against real silicon rather than against the manuals. No code is derived
from Bochs, QEMU, SoftFloat or any other emulator; everything here is under
the [MIT license](LICENSE), including the tests.

* Complete programmer-visible state: data registers, control / status / tag
  words, TOP, FIP / FDP / FOP, with FNSAVE / FRSTOR / FNSTENV / FLDENV images
  (16- and 32-bit, protected and real mode layouts) and FXSAVE / FXRSTOR.
* Every instruction in `D8..DF`, including FISTTP, FCMOVcc, F[U]COMI[P] and
  the undocumented aliases (FFREEP, `D9 D8+i`, `DC D0+i`, `DF C8+i`, ...).
* Own 80-bit arithmetic: precision control (24 / 53 / 64 bits), all four
  rounding modes, the six exceptions with masked *and* unmasked responses
  (no pop / no store, ±24576 exponent bias adjustment), stack faults, the C1
  "rounded up" indication, x87 NaN propagation, and the pseudo-denormal /
  unnormal / pseudo-NaN / pseudo-infinity encodings.
* Transcendentals with Intel's 66-bit π argument reduction and a 192-bit
  internal kernel.
* Two API levels: an instruction interpreter (`x87_exec`) and the
  value-level operations it is built on (`x87_ops.h`) for binary translators
  that track the register stack themselves.
* No allocation, no global state, no host floating point, no libc beyond
  `<stdint.h>` / `<stddef.h>`. Uses `unsigned __int128` where the compiler
  has it and portable 64-bit arithmetic otherwise (32-bit targets work).

### How faithful is it?

`tests/hostdiff` generates random FPU states (heavily biased towards corner
cases: denormals, unsupported encodings, NaNs, empty registers, unmasked
exceptions, pending faults, rounding boundaries) and random instructions,
executes each one both on the host CPU and in the emulator, and compares the
**entire** resulting state: all eight registers, status / control / tag
words, the memory operand, EFLAGS, AX, and whether `#MF` or `#UD` was raised.

Reference CPU: Intel Xeon, family 6 model 207 (Emerald Rapids).

| Instructions | Result |
|---|---|
| Everything except the eight transcendentals | **bit-identical** — 0 mismatches in tens of millions of random cases |
| Transcendentals: flags, condition codes, stack behaviour, special and out-of-range operands | **bit-identical** — 0 mismatches |
| Transcendentals: result value | within **1 ulp** always; bit-identical rate below |

Bit-identical rate of transcendental results on ordinary in-range operands,
all four rounding modes (`tests/hostdiff -A`):

| | bit-identical |
|---|---|
| FYL2X | 99.5 % |
| FSIN, FCOS | 97.8 % |
| FPATAN | 96.5 % |
| FSINCOS (both results) | 96.3 % |
| FPTAN | 94.0 % |
| F2XM1 | 93.1 % |
| FYL2XP1 | 90.8 % |

The remaining differences are the limit of black-box modelling: the CPU's
microcode evaluates these functions with a ~67-bit internal format and is
itself not correctly rounded. Where a model of that internal behaviour could
be established from measurements it is implemented (FYL2X truncates
log2(x) to 67 bits before multiplying by y — this alone raises its match
rate from 93 % to 99.5 %); elsewhere the library returns the correctly
rounded value of the function the hardware approximates. Intel and AMD
differ from each other in exactly these cases, so no single implementation
can match both.

Hardware behaviours that were found by measurement and deviate from, or are
not spelled out in, the SDM — all reproduced:

* Transcendentals ignore precision control and always report `PE`.
* FSIN/FCOS return `x` / `1.0` unchanged below 2^-68, FPTAN below 2^-33,
  regardless of rounding mode; FSIN(π) etc. follow the 66-bit π exactly.
* F2XM1 with |x| > 1 and FYL2XP1 with x ≤ -1 return the operand unchanged.
* FYL2X of 2^-n comes out a hair above `-n` (visible in directed rounding).
* An unmasked `#D` aborts arithmetic but not FLD m32/m64.
* A denormal divided by zero raises only `#Z`.
* FSCALE and FPREM never apply precision control; FSCALE with an unmasked
  over/underflow that is out of range even after the bias adjustment returns
  ±∞ / ±0 irrespective of rounding mode.
* FPREM/FPREM1 leave C0/C3 alone on invalid operands; pseudo-denormals passed
  through by FPREM/FSCALE come out normalised.
* F[U]COMI[P] and FCMOVcc leave C1 alone; FFREE/FFREEP clear it.
* `D9 D8+i` (FSTP alias) does not raise a stack fault on an empty ST(0).

### Using it

```c
#include "x87.h"

x87_state fpu;
x87_init(&fpu);

double a = 1.0, b = 3.0, q;
x87_insn in = { 0 };

in.op = 0xDD; in.modrm = 0x00; in.mem = &a;   /* fld  qword [a]  */
x87_exec(&fpu, &in);
in.op = 0xDC; in.modrm = 0x30; in.mem = &b;   /* fdiv qword [b]  */
x87_exec(&fpu, &in);
in.op = 0xDD; in.modrm = 0x18; in.mem = &q;   /* fstp qword [q]  */
int r = x87_exec(&fpu, &in);                  /* r & X87_R_MEM_WRITE */
```

The embedding CPU emulator decodes prefixes and the effective address, then
hands over the two opcode bytes and a pointer to the operand
(`x87_mem_operand()` gives its size and direction). `x87_exec()` returns
`X87_R_MF` *without executing* when an unmasked exception is pending — raise
`#MF` (CR0.NE=1) or assert FERR# (`x87_pending()`), exactly as the hardware
would — and `X87_R_UD` for invalid encodings. See [`include/x87.h`](include/x87.h).

### Value-level API for lifters / recompilers

If your translator already models the x87 stack and data movement, use the
operations directly ([`include/x87_ops.h`](include/x87_ops.h)) — these are the
very functions `x87_exec()` calls, so they carry the same hardware-verified
behaviour:

```c
#include "x87_ops.h"

x87_env e = { .cw = cw };                 /* rounding, precision, masks */
x87_operand a = x87_operand_f80(st0);
x87_operand b = x87_operand_f64(mem_bits);   /* FADD m64real */
x87_f80 r = x87_add(&a, &b, &e);

sw |= e.flags;                            /* IE..PE raised by the operation */
if (!e.inhibit) {                         /* unmasked #I/#Z/#D: no result */
    st0 = r;
    /* C1 = e.c1 */
}
```

* Every operation overwrites `flags`, `c1` and `inhibit`; only `cw` is input.
  `inhibit` tells you an unmasked exception suppressed the result (leave the
  destination alone, do not pop). Unmasked overflow/underflow results come
  back bias-adjusted, as on hardware. No build flag is involved: pass a
  constant `0x037F` and ignore the outputs if you do not model exceptions.
* Operands are built with the constructor matching their source
  (`x87_operand_f80/_f32/_f64/_int`), because arithmetic with a memory
  operand is not the same as "convert, then operate" for denormals and
  signalling NaNs.
* Covered: add/sub/mul/div, sqrt, rndint, scale, prem/prem1, xtract, compare,
  xam, loads and stores for f32/f64/i16/i32/i64/BCD, the constants, and all
  transcendentals.
* `src/x87_amalgam.c` builds everything as one translation unit with the
  internals `static`; `make x87.bc` produces LLVM bitcode from it. Define
  `X87_API` (e.g. `static inline __attribute__((always_inline))` when
  including the amalgamation from your semantics file) to have the public
  wrappers inlined into lifted code. The wrappers and their exception
  resolution inline and fold; the rounding core (`x87i_add`, ...) is large
  and stays an out-of-line call unless you force it via `X87_INTERNAL`.
* The C signatures pass `x87_f80` (`{i64, i16}`) by value, which Clang lowers
  differently per target ABI. A pass that emits calls by hand should call
  small wrappers from its own C semantics file rather than these directly.
* Define `X87_NO_INT128` to force the portable arithmetic; it is selected
  automatically where `__int128` does not exist and is verified against the
  CPU and the recorded vectors just like the default build.

### Building and testing

```sh
make                # libx87.a
make check          # portable: value-API test + replay of vectors recorded on the
                    # reference CPU (also built without __int128)
make check-host     # x86-64 only: differential test against *your* CPU
tests/hostdiff -n 20000000 -s 42     # more cases, other seed
tests/hostdiff -A                    # transcendental bit-exact report
```

`make check` works on any architecture: `tests/vectors/*.bin` holds ~5,900
cases recorded with `hostdiff -G`, and `tests/replay` requires the emulator
to reproduce the recorded hardware state. Running `make check-host` on an
AMD or older Intel machine shows precisely where that CPU differs from the
reference.

### Scope and limitations

* Models a P6-and-later Intel FPU. 8087 / 287 specifics (projective
  infinity, acceptance of unnormals, FENI/FDISI) are not modelled; those
  opcodes are no-ops as on modern CPUs.
* Validated against a single Intel microarchitecture. AMD transcendentals
  differ in the last bit in a few percent of cases.
* FIP / FDP / FOP are recorded from what the caller supplies for every
  non-control instruction (the classic behaviour); the "FOP only on unmasked
  exception" and FCS/FDS-deprecation modes of recent CPUs are not modelled.
  These fields, and the real-mode environment layouts, could not be compared
  against hardware from a 64-bit user-mode process.
* The existing emulator test suites (QEMU `tests/tcg/i386`, Bochs) were not
  used: they are self-checking programs for the machine they run on, and a
  direct differential test against silicon is strictly stronger.

### Layout

| | |
|---|---|
| `include/x87.h` | interpreter API (state, `x87_exec`) |
| `include/x87_ops.h` | value-level operations |
| `src/x87_arith.c` | 80-bit arithmetic, rounding, conversions |
| `src/x87_trans.c` | transcendental kernels |
| `src/x87_ops.c` | public value-level operations, exception resolution |
| `src/x87_exec.c` | decoder, stack / tag handling, state images |
| `src/x87_amalgam.c` | single translation unit build |
| `tests/ops_test.c` | portable test of the value-level API |
| `tests/hostdiff.c` | differential tester against the host CPU |
| `tests/replay.c`, `tests/vectors/` | portable replay of recorded hardware vectors |
