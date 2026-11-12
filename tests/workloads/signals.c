/* SPDX-License-Identifier: MIT
 *
 * Workload for x87trace: x87 code before, inside and after signal handlers,
 * including a handled SIGFPE from an unmasked x87 exception.  The tracer
 * must keep checking instructions across signal delivery, and must deliver a
 * SIGTRAP the program raises itself rather than mistake it for its own
 * single-step trap: it should report 31 x87 instructions.
 *
 * build: cc -O1 -static -no-pie signals.c
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <ucontext.h>

static volatile long double sink;
static volatile int fpe_seen;

static void on_usr1(int sig)
{
    long double x = 3.0L;
    (void)sig;
    __asm__ volatile("fldt %0\n fsqrt\n fstpt %0" : "+m"(x)); /* 3 instructions */
    sink = x;                                                /* fld + fstp: 2 */
}

static volatile int trap_seen;

static void on_trap(int sig)
{
    long double x = 2.0L;
    (void)sig;
    trap_seen++;
    __asm__ volatile("fldt %0\n fsqrt\n fstpt %0" : "+m"(x)); /* 3 instructions */
}

static void on_fpe(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    (void)sig; (void)si;
    fpe_seen = 1;
    /* clear the pending exception and mask everything in the saved state */
    uc->uc_mcontext.fpregs->swd &= 0x7F00;
    uc->uc_mcontext.fpregs->cwd |= 0x3F;
}

int main(void)
{
    struct sigaction sa = { 0 };
    long double a = 1.0L, b = 0.0L;
    unsigned short cw = 0x037B; /* unmask #Z */

    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, 0);
    sa.sa_handler = on_trap;
    sigaction(SIGTRAP, &sa, 0);
    sa.sa_sigaction = on_fpe;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGFPE, &sa, 0);

    __asm__ volatile("fldt %0\n fstpt %0" : "+m"(a));         /* 2 */
    raise(SIGUSR1);                                          /* 5 in the handler */
    __asm__ volatile("fldt %0\n fstpt %0" : "+m"(a));         /* 2 */

    /* 1/0 with #Z unmasked: the fwait-class fstp faults, the handler masks
     * the exception and the instruction is restarted */
    __asm__ volatile("fldcw %2\n fldt %0\n fldt %1\n fdivrp\n fstpt %0\n fninit" : "+m"(a) : "m"(b), "m"(cw));
    __asm__ volatile("fldt %0\n fstpt %0" : "+m"(a));         /* 2 */
    raise(SIGTRAP);                                          /* 3 in the handler */
    __asm__ volatile("fldt %0\n fstpt %0" : "+m"(a));         /* 2 */
    printf("fpe_seen=%d trap_seen=%d\n", fpe_seen, trap_seen);
    return !(fpe_seen && trap_seen == 1);
}
