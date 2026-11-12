CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Iinclude
SRC = src/x87_arith.c src/x87_trans.c src/x87_ops.c src/x87_exec.c
OBJ = $(SRC:.c=.o)

all: libx87.a

libx87.a: $(OBJ)
	$(AR) rcs $@ $^

src/%.o: src/%.c include/x87.h include/x87_ops.h src/x87_internal.h src/x87_tables.h
	$(CC) $(CFLAGS) -c $< -o $@

tests/hostdiff: tests/hostdiff.c libx87.a
	$(CC) $(CFLAGS) -D_GNU_SOURCE $< libx87.a -o $@

tests/replay: tests/replay.c libx87.a
	$(CC) $(CFLAGS) $< libx87.a -o $@

tests/ops_test: tests/ops_test.c libx87.a
	$(CC) $(CFLAGS) $< libx87.a -o $@

tests/state_test: tests/state_test.c libx87.a
	$(CC) $(CFLAGS) $< libx87.a -o $@

# The same tests built as a single translation unit with the portable
# 64-bit arithmetic (no unsigned __int128).
tests/replay-noint128: tests/replay.c $(SRC) src/x87_amalgam.c include/x87.h include/x87_ops.h src/x87_internal.h
	$(CC) $(CFLAGS) -DX87_NO_INT128 tests/replay.c src/x87_amalgam.c -o $@

tests/x87trace: tests/x87trace.c libx87.a
	$(CC) $(CFLAGS) $< libx87.a -o $@

tests/bench: tests/bench.c libx87.a
	$(CC) $(CFLAGS) $< libx87.a -o $@

bench: tests/bench
	tests/bench

# x86-64 Linux only: check every x87 instruction executed by real programs
# (QEMU's tcg tests, glibc's long double libm, 387 code generation) against
# the CPU.
tests/workloads/libm_ld: tests/workloads/libm_ld.c
	$(CC) -O1 -static -no-pie -w $< -o $@ -lm

tests/workloads/fp387: tests/workloads/fp387.c
	$(CC) -O2 -mfpmath=387 -static -no-pie -w $< -o $@ -lm

tests/workloads/signals: tests/workloads/signals.c
	$(CC) -O1 -static -no-pie -w $< -o $@

check-trace: tests/x87trace tests/workloads/fp387 tests/workloads/libm_ld tests/workloads/signals
	tests/x87trace -q tests/workloads/signals | grep ' 31 x87 instructions .* 0 mismatches'
	tests/trace-qemu.sh $(TRACE_WORKDIR)
	tests/x87trace -q tests/workloads/fp387
	tests/x87trace -q -n 300000 tests/workloads/libm_ld 2

# LLVM bitcode of the whole library, for linking into lifted code.
x87.bc: $(SRC) src/x87_amalgam.c include/x87.h include/x87_ops.h src/x87_internal.h
	clang -O2 -std=c11 -Iinclude -emit-llvm -c src/x87_amalgam.c -o $@

# Portable: replay vectors recorded on real hardware.
check: tests/replay tests/replay-noint128 tests/ops_test tests/state_test
	tests/ops_test
	tests/state_test
	tests/replay tests/vectors/*.bin
	tests/replay-noint128 tests/vectors/*.bin

# x86-64 hosts only: differential test against the host CPU.
check-host: tests/hostdiff
	tests/hostdiff -n 5000000
	tests/hostdiff -A -n 800000

clean:
	rm -f $(OBJ) libx87.a x87.bc tests/hostdiff tests/replay tests/replay-noint128 tests/ops_test tests/state_test \
	      tests/x87trace tests/bench tests/workloads/libm_ld tests/workloads/fp387 tests/workloads/signals

.PHONY: all check check-host check-trace bench clean
