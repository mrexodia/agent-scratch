/* SPDX-License-Identifier: MIT
 *
 * Workload for x87trace: ordinary float/double code compiled for the x87
 * (as old 32-bit games were): mixed precision, FXCH-heavy expression
 * evaluation, integer conversions with control word switching, compares.
 *
 * build: cc -O2 -mfpmath=387 -mno-sse2 ... is not possible on x86-64, so:
 *        cc -O2 -mfpmath=387 -static -no-pie fp387.c -lm
 */
#include <math.h>
#include <stdio.h>

typedef struct { float x, y, z; } vec3;

static float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static vec3 cross(vec3 a, vec3 b) { vec3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; return r; }
static vec3 norm(vec3 a) { float l = sqrtf(dot(a, a)); vec3 r = { a.x / l, a.y / l, a.z / l }; return r; }

static void mat_mul(float r[16], const float a[16], const float b[16])
{
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
}

static int mandel(double cr, double ci)
{
    double zr = 0, zi = 0;
    int n = 0;
    while (n < 60 && zr * zr + zi * zi < 4.0) {
        double t = zr * zr - zi * zi + cr;
        zi = 2 * zr * zi + ci;
        zr = t;
        n++;
    }
    return n;
}

static void nbody(double p[][3], double v[][3], const double *m, int n, double dt)
{
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            double d[3], r2 = 0, mag;
            for (int k = 0; k < 3; k++) { d[k] = p[i][k] - p[j][k]; r2 += d[k] * d[k]; }
            mag = dt / (r2 * sqrt(r2));
            for (int k = 0; k < 3; k++) { v[i][k] -= d[k] * m[j] * mag; v[j][k] += d[k] * m[i] * mag; }
        }
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) p[i][k] += dt * v[i][k];
}

static unsigned rs = 12345;
static float frand(void) { rs = rs * 1103515245u + 12345u; return (float)(rs >> 8) / 16777216.0f - 0.5f; }

int main(void)
{
    volatile float facc = 0;
    volatile double dacc = 0;
    volatile long lacc = 0;
    float A[16], B[16], C[16];
    double p[4][3], v[4][3], m[4] = { 1.0, 0.3, 0.01, 2.5 };

    for (int it = 0; it < 40; it++) {
        vec3 a = { frand(), frand(), frand() }, b = { frand(), frand(), frand() };
        vec3 c = norm(cross(a, b));
        facc += dot(c, a) + dot(c, b) + c.x;
        for (int i = 0; i < 16; i++) { A[i] = frand() * 4; B[i] = frand() * 4; }
        mat_mul(C, A, B);
        for (int i = 0; i < 16; i++) {
            facc += C[i];
            lacc += (int)(C[i] * 100.0f) + (short)(A[i] * 50.0f) + lrintf(B[i] * 1000.0f) + (long long)(C[i] * 1e6f);
        }
        dacc += sin(facc) * cos(a.x) + atan2(a.y, b.z) + exp(a.z) + log(fabs(b.x) + 1e-3) + pow(fabs(a.x) + 0.5, b.y) +
                fmod(C[3], 0.37) + floor(C[5]) + ceil(C[6]) + sqrt(fabs(C[7]));
        if (a.x > b.x) dacc += 1; else if (a.y <= b.y) dacc -= 1;
        if (c.z != c.z) lacc++;
    }
    for (int y = -6; y < 6; y++)
        for (int x = -10; x < 6; x++)
            lacc += mandel(x * 0.15, y * 0.18);
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++) { p[i][k] = frand() * 10; v[i][k] = frand(); }
    for (int s = 0; s < 60; s++) nbody(p, v, m, 4, 0.01);
    dacc += p[0][0] + v[3][2];
    printf("%g %g %ld\n", (double)facc, (double)dacc, (long)lacc);
    return 0;
}
