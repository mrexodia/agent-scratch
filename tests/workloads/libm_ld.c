/* SPDX-License-Identifier: MIT
 *
 * Workload for x87trace: drives glibc's long double math (implemented with
 * x87 instructions on x86) over edge-case operands, in every rounding mode
 * and precision setting.
 *
 * build: cc -O1 -static -no-pie libm_ld.c -lm
 */
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static volatile long double sink;
static volatile long long isink;

static const long double edge[] = {
    0.0L, -0.0L, 1.0L, -1.0L, 2.0L, 0.5L, 3.0L, 10.0L, 1e-5L, 1e5L, 0.1L, 0.7L, 1.5L, 2.5L, -2.5L, 3.5L,
    0x1p-16382L, 0x1p-16445L, 0x1.8p-16400L, LDBL_MAX, -LDBL_MAX, LDBL_MIN, LDBL_EPSILON, 1.0L + LDBL_EPSILON,
    1.0L - LDBL_EPSILON / 2, 0x1p63L, 0x1p63L - 1, 0x1p64L, -0x1p63L, 0x1p31L, 0x1p15L, 0x1.fffffffffffffffep62L,
    3.14159265358979323846264338327950288L, 1.57079632679489661923132169163975144L, 0.78539816339744830961566084581987572L,
    2.71828182845904523536028747135266250L, 0.69314718055994530941723212145817657L, 1e300L, 1e-300L, 1e4000L, 1e-4000L,
    123456789.125L, 0x1.000002p0L, 0x1.0000000000001p0L, 0x1.fffffep127L, 0x1.fffffffffffffp1023L, 0x1p-149L, 0x1p-1074L,
    __builtin_infl(), -__builtin_infl(), __builtin_nanl(""), -__builtin_nanl("0x1234"),
};
#define N (sizeof edge / sizeof *edge)

static unsigned long long rs = 88172645463325252ull;
static long double rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return ldexpl((long double)(long long)rs, (int)(rs % 140) - 130);
}

static void one(long double x, long double y)
{
    int e;
    long double ip;
    sink = x + y; sink = x - y; sink = x * y; sink = x / y;
    sink = sqrtl(x); sink = fabsl(x); sink = -x;
    sink = sinl(x); sink = cosl(x); sink = tanl(x);
    sink = atanl(x); sink = atan2l(y, x); sink = asinl(x); sink = acosl(x);
    sink = expl(x); sink = exp2l(x); sink = exp10l(x); sink = expm1l(x);
    sink = logl(x); sink = log2l(x); sink = log10l(x); sink = log1pl(x);
    sink = powl(x, y); sink = hypotl(x, y); sink = cbrtl(x);
    sink = sinhl(x); sink = coshl(x); sink = tanhl(x); sink = asinhl(x); sink = acoshl(x); sink = atanhl(x);
    sink = fmodl(x, y); sink = remainderl(x, y); sink = remquol(x, y, &e);
    sink = rintl(x); sink = nearbyintl(x); sink = floorl(x); sink = ceill(x); sink = truncl(x); sink = roundl(x);
    sink = scalbnl(x, 7); sink = scalbnl(x, -20000); sink = ldexpl(x, 20000); sink = logbl(x);
    sink = frexpl(x, &e); sink = modfl(x, &ip); sink = fmal(x, y, x);
    isink = llrintl(x); isink = lrintl(x); isink = (long long)x; isink = (int)x; isink = (short)x;
    isink = ilogbl(x);
    sink = (long double)(double)x; sink = (long double)(float)x;
    sink = (long double)(float)y * x; sink = (long double)(double)y + x;
    isink = x < y; isink = x == y; isink = isgreater(x, y); isink = isunordered(x, y);
    isink = fpclassify(x); isink = signbit(x);
}

int main(int argc, char **argv)
{
    static const int modes[4] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
    static const unsigned short pcs[3] = { 0x0300, 0x0200, 0x0000 };
    int nrand = argc > 1 ? atoi(argv[1]) : 10;
    char buf[64];

    for (int p = 0; p < 3; p++)
        for (int m = 0; m < 4; m++) {
            unsigned short cw;
            fesetround(modes[m]);
            __asm__ volatile("fnstcw %0" : "=m"(cw));
            cw = (unsigned short)((cw & ~0x0300) | pcs[p]);
            __asm__ volatile("fldcw %0" : : "m"(cw));
            if (p == 0)
                for (unsigned i = 0; i < N; i++)
                    for (unsigned j = (i + (unsigned)m) % 7; j < N; j += (m ? 13 : 7))
                        one(edge[i], edge[j]);
            for (int i = 0; i < nrand; i++)
                one(rnd(), rnd());
            feclearexcept(FE_ALL_EXCEPT);
        }
    fesetround(FE_TONEAREST);
    __asm__ volatile("fninit");
    /* decimal conversions */
    for (unsigned i = 0; i < N; i++) {
        snprintf(buf, sizeof buf, "%.25Lg", edge[i]);
        sink = strtold(buf, 0);
    }
    puts("done");
    return 0;
}
