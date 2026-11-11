CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Iinclude
SRC = src/x87_arith.c src/x87_trans.c src/x87_ops.c src/x87_exec.c
OBJ = $(SRC:.c=.o)

all: libx87.a

libx87.a: $(OBJ)
	$(AR) rcs $@ $^

src/%.o: src/%.c include/x87.h include/x87_ops.h src/x87_internal.h
	$(CC) $(CFLAGS) -c $< -o $@

tests/hostdiff: tests/hostdiff.c libx87.a
	$(CC) $(CFLAGS) -D_GNU_SOURCE $< libx87.a -o $@

tests/replay: tests/replay.c libx87.a
	$(CC) $(CFLAGS) $< libx87.a -o $@

tests/ops_test: tests/ops_test.c libx87.a
	$(CC) $(CFLAGS) $< libx87.a -o $@

# The same tests built as a single translation unit with the portable
# 64-bit arithmetic (no unsigned __int128).
tests/replay-noint128: tests/replay.c $(SRC) src/x87_amalgam.c include/x87.h include/x87_ops.h src/x87_internal.h
	$(CC) $(CFLAGS) -DX87_NO_INT128 tests/replay.c src/x87_amalgam.c -o $@

# LLVM bitcode of the whole library, for linking into lifted code.
x87.bc: $(SRC) src/x87_amalgam.c include/x87.h include/x87_ops.h src/x87_internal.h
	clang -O2 -std=c11 -Iinclude -emit-llvm -c src/x87_amalgam.c -o $@

# Portable: replay vectors recorded on real hardware.
check: tests/replay tests/replay-noint128 tests/ops_test
	tests/ops_test
	tests/replay tests/vectors/*.bin
	tests/replay-noint128 tests/vectors/*.bin

# x86-64 hosts only: differential test against the host CPU.
check-host: tests/hostdiff
	tests/hostdiff -n 5000000
	tests/hostdiff -A -n 800000

clean:
	rm -f $(OBJ) libx87.a x87.bc tests/hostdiff tests/replay tests/replay-noint128 tests/ops_test

.PHONY: all check check-host clean
