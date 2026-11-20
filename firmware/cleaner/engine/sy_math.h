/* SPDX-License-Identifier: GPL-3.0-only */
/* Small float math for the engine: no libm, so the same code runs on the host, in a plugin and on the FM-1
 * (where clang has no float library functions). All single precision. */
#ifndef SY_MATH_H
#define SY_MATH_H
/* (the FM-1 build with the voice in RAM: these always inlined, so RAM code calls nothing in the flash) */
#ifdef SY_HOT_RAM
#define SY_MATHINL static inline __attribute__((always_inline))
#else
#define SY_MATHINL static inline
#endif
#include <stdint.h>

#define SY_PI 3.14159265f
#define SY_2PI 6.28318531f

typedef union { float f; int32_t i; uint32_t u; } sy_fu;

SY_MATHINL int sy_floori(float x) { int i = (int)x; return (x < (float)i) ? i - 1 : i; }
SY_MATHINL float sy_absf(float x) { sy_fu u; u.f = x; u.u &= 0x7FFFFFFFu; return u.f; }
SY_MATHINL float sy_minf(float a, float b) { return a < b ? a : b; }
SY_MATHINL float sy_maxf(float a, float b) { return a > b ? a : b; }
SY_MATHINL float sy_clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* 2^x, relative error < 2e-7 over the float range used here (|x| < 126) */
SY_MATHINL float sy_exp2(float x)
{
    int i;
    float f, p;
    sy_fu u;
    if (x < -126.f) x = -126.f;
    if (x > 126.f) x = 126.f;
    i = sy_floori(x);
    f = x - (float)i;
    p = 1.0f + f * (0.693147182f + f * (0.240226507f + f * (0.0555041087f + f * (0.00961812911f
        + f * (0.00133335581f + f * 0.000154035304f)))));
    u.i = (i + 127) << 23;
    return p * u.f;
}

/* log2(x) for x > 0, absolute error < 1e-6 */
SY_MATHINL float sy_log2(float x)
{
    sy_fu u;
    int e;
    float m, t, t2;
    u.f = x;
    e = (int)((u.u >> 23) & 0xFF) - 127;
    u.u = (u.u & 0x007FFFFFu) | 0x3F800000u;   /* mantissa in [1, 2) */
    m = u.f;
    if (m > 1.41421356f) { m *= 0.5f; e++; }
    t = (m - 1.f) / (m + 1.f);                  /* ln(m) = 2 atanh(t) */
    t2 = t * t;
    return (float)e + 2.885390082f * t * (1.f + t2 * (0.333333333f + t2 * (0.2f + t2 * (0.142857143f + t2 * 0.111111111f))));
}

SY_MATHINL float sy_db2lin(float db) { return sy_exp2(db * 0.166096405f); }       /* 10^(db/20) */
SY_MATHINL float sy_lin2db(float a) { return 6.020599913f * sy_log2(a); }

/* sin(2 pi t) for any t (cycles), error < 2e-7 */
SY_MATHINL float sy_sin2pi(float t)
{
    float x, x2;
    t -= (float)sy_floori(t + 0.5f);            /* -0.5 .. 0.5 */
    if (t > 0.25f) t = 0.5f - t;
    else if (t < -0.25f) t = -0.5f - t;
    x = t * SY_2PI;
    x2 = x * x;
    return x * (1.f + x2 * (-0.166666667f + x2 * (0.00833333333f + x2 * (-0.000198412698f
        + x2 * (2.75573192e-6f - x2 * 2.50521084e-8f)))));
}
SY_MATHINL float sy_cos2pi(float t) { return sy_sin2pi(t + 0.25f); }

/* tan(pi * f) for f in [0, 0.5) (the prewarped filter coefficient) */
SY_MATHINL float sy_tanpi(float f) { return sy_sin2pi(0.5f * f) / sy_cos2pi(0.5f * f); }

/* tanh-like odd soft clip (a rational approximation, exact limits +-1) */
SY_MATHINL float sy_tanh(float x)
{
    float x2;
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

/* a fast 32-bit noise generator */
SY_MATHINL uint32_t sy_rand(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }
SY_MATHINL float sy_randf(uint32_t *s) { return (float)(int32_t)sy_rand(s) * 4.65661287e-10f; }   /* -1..1 */


/* S1: code in the FM-1's flash (setup, table builders, the effects' and the reverb's parameters: not per sample, not in
 * the RAM code) calls the transcendentals as shared functions (sy_tables.c, SY_COLD: the same arithmetic as the inline
 * ones below, so the same bits) instead of an inlined copy at every site: the firmware's SY_MATHINL is always_inline.
 * Only for code in the flash: the RAM code has no call into the flash (it keeps the inline ones, or ctl_exp2 / ctl_log2
 * in synth.c). Other builds: the inline ones. */
#ifdef SY_HOT_RAM
float sy_exp2_f(float x);
float sy_log2_f(float x);
float sy_sin2pi_f(float t);
float sy_cos2pi_f(float t);
float sy_tanpi_f(float f);
#define FEXP2(x) sy_exp2_f(x)
#define FLOG2(x) sy_log2_f(x)
#define FSIN2PI(x) sy_sin2pi_f(x)
#define FCOS2PI(x) sy_cos2pi_f(x)
#define FTANPI(x) sy_tanpi_f(x)
#define FDB2LIN(x) sy_exp2_f((x) * 0.166096405f)   /* (sy_db2lin) */
#define FLIN2DB(x) (6.020599913f * sy_log2_f(x))   /* (sy_lin2db) */
#else
#define FEXP2(x) sy_exp2(x)
#define FLOG2(x) sy_log2(x)
#define FSIN2PI(x) sy_sin2pi(x)
#define FCOS2PI(x) sy_cos2pi(x)
#define FTANPI(x) sy_tanpi(x)
#define FDB2LIN(x) sy_db2lin(x)
#define FLIN2DB(x) sy_lin2db(x)
#endif

#endif
