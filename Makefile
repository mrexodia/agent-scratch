CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Iinclude
SRC = src/x87_arith.c src/x87_exec.c src/x87_trans.c
OBJ = $(SRC:.c=.o)

all: libx87.a

libx87.a: $(OBJ)
	$(AR) rcs $@ $^

src/%.o: src/%.c include/x87.h src/x87_internal.h
	$(CC) $(CFLAGS) -c $< -o $@

tests/hostdiff: tests/hostdiff.c libx87.a
	$(CC) $(CFLAGS) -D_GNU_SOURCE $< libx87.a -o $@

tests/replay: tests/replay.c libx87.a
	$(CC) $(CFLAGS) $< libx87.a -o $@

# Portable: replay vectors recorded on real hardware.
check: tests/replay
	tests/replay tests/vectors/*.bin

# x86-64 hosts only: differential test against the host CPU.
check-host: tests/hostdiff
	tests/hostdiff -n 5000000
	tests/hostdiff -A -n 800000

clean:
	rm -f $(OBJ) libx87.a tests/hostdiff tests/replay

.PHONY: all check check-host clean
