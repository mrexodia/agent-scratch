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

check: tests/hostdiff
	tests/hostdiff -n 2000000

clean:
	rm -f $(OBJ) libx87.a tests/hostdiff

.PHONY: all check clean
