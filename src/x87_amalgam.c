/* SPDX-License-Identifier: MIT
 *
 * Single translation unit build of the whole library.
 *
 * Useful for producing one object / LLVM bitcode module that a translator
 * links its lifted code against and then inlines, e.g.
 *
 *   clang -O2 -emit-llvm -c src/x87_amalgam.c -Iinclude -o x87.bc
 *
 * The internal functions are static here, so the optimiser can inline and
 * specialise them.  Define X87_API to control the public functions, e.g.
 * include this file from your own semantics source after
 *
 *   #define X87_API static inline __attribute__((always_inline))
 *
 * to have the operations inlined into the lifted code.
 */
#ifndef X87_INTERNAL
#define X87_INTERNAL static
#endif

#include "x87_arith.c"
#include "x87_trans.c"
#include "x87_ops.c"
#include "x87_exec.c"
