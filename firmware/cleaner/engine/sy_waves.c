/* SPDX-License-Identifier: GPL-3.0-only */
/* The oscillator waves 2-63, designed for this engine from the coarse character sheet: only
 * the octave-band energies (3 dB steps) and the words are used, never harmonic amplitudes or phases of the
 * reference (there are none in this repository). The method is this project's own: within each octave band the
 * band's energy is spread over its harmonics with a tilt chosen by the words (smooth: steep, buzzy: flat with a
 * seeded jitter, else 1/n), weighted by the parity words (mostly odd / even dominate), with seeded phases. Each
 * wave is built as SY_WAVE_LEVELS band-limited tables (64, 32, ... 1 harmonics) at its measured level.
 * Since 0.90 these designed waves are the factory waves of the FM-1 builds (they replace the measured tables, which
 * are gone); a slot of the user wave bank (sy_user_wave, the app's) replaces one by the
 * player's own 64 harmonics. */
#include "sy_int.h"

const uint8_t *(*sy_user_wave)(int id);  /* the app's user wave bank: id's record (SY_UWAVE_LEN bytes) or 0 */

/* waves 2-63: octave-band energies (dB re the total; h1, h2-3, h4-7, h8-15, h16-31, h32-64), character flags
 * (1 mostly odd, 2 even dominate, 4 few/smooth, 8 dense/buzzy) and the brightness, from the development notes */
static const int8_t WAVE_BANDS[62][6] = {
    {0, -9, -18, -18, -24, -15},   /* 2 */
    {-3, -6, -9, -9, -15, -27},   /* 3 */
    {-3, -6, -12, -15, -18, -24},   /* 4 */
    {-6, -9, -3, -12, -15, -21},   /* 5 */
    {-9, -3, -6, -9, -18, -18},   /* 6 */
    {-6, -3, -12, -12, -9, -30},   /* 7 */
    {0, -9, -27, -18, -27, -30},   /* 8 */
    {-3, -3, -15, -12, -18, -51},   /* 9 */
    {-3, -3, -15, -18, -24, -27},   /* 10 */
    {-3, -3, -18, -15, -27, -27},   /* 11 */
    {-6, -15, -3, -15, -9, -12},   /* 12 */
    {-3, -45, -3, -18, -36, -27},   /* 13 */
    {-51, -33, 0, -9, -9, -24},   /* 14 */
    {-30, -6, -3, -6, -18, -30},   /* 15 */
    {-6, -15, -6, -9, -9, -12},   /* 16 */
    {-21, -3, -9, -9, -9, -18},   /* 17 */
    {-3, -6, -15, -6, -15, -42},   /* 18 */
    {-21, -9, -3, -9, -9, -18},   /* 19 */
    {-9, -6, -3, -12, -12, -15},   /* 20 */
    {-6, -6, -3, -9, -12, -24},   /* 21 */
    {-9, -3, -6, -12, -15, -24},   /* 22 */
    {-24, -15, -6, -6, -6, -6},   /* 23 */
    {-3, -6, -15, -15, -15, -12},   /* 24 */
    {-3, -6, -9, -18, -15, -27},   /* 25 */
    {-18, -6, -3, -9, -9, -9},   /* 26 */
    {-21, -9, -3, -9, -9, -12},   /* 27 */
    {-12, -6, -6, -6, -9, -18},   /* 28 */
    {-6, -3, -9, -9, -33, -66},   /* 29 */
    {-3, -12, -12, -6, -21, -39},   /* 30 */
    {0, -9, -12, -15, -36, -39},   /* 31 */
    {-3, -15, -6, -12, -21, -36},   /* 32 */
    {0, -9, -15, -12, -21, -36},   /* 33 */
    {0, -27, -12, -15, -18, -54},   /* 34 */
    {-9, -3, -15, -12, -15, -15},   /* 35 */
    {0, -12, -12, -21, -21, -27},   /* 36 */
    {-6, -6, -6, -6, -18, -36},   /* 37 */
    {-3, -6, -18, -18, -18, -33},   /* 38 */
    {-3, -6, -12, -12, -18, -33},   /* 39 */
    {-3, -9, -6, -12, -15, -18},   /* 40 */
    {-54, -3, -6, -6, -18, -39},   /* 41 */
    {-54, 0, -15, -9, -27, -33},   /* 42 */
    {-54, -3, -9, -9, -3, -21},   /* 43 */
    {-39, -6, -3, -39, -30, -39},   /* 44 */
    {-66, -6, -9, -6, -3, -15},   /* 45 */
    {-72, -9, -6, -12, -3, -21},   /* 46 */
    {-9, -6, -3, -12, -12, -18},   /* 47 */
    {-60, -3, -12, -6, -6, -18},   /* 48 */
    {-3, -15, -9, -6, -9, -18},   /* 49 */
    {-3, -36, -6, -6, -12, -12},   /* 50 */
    {-66, -42, 0, -21, -15, -9},   /* 51 */
    {-36, -3, -30, -3, -9, -33},   /* 52 */
    {-3, -9, -12, -6, -9, -15},   /* 53 */
    {-3, -9, -21, -12, -6, -6},   /* 54 */
    {-51, -9, -15, -3, -6, -6},   /* 55 */
    {-36, -18, -3, -9, -9, -15},   /* 56 */
    {-24, -12, -6, -9, -3, -18},   /* 57 */
    {-24, -3, -12, -6, -9, -9},   /* 58 */
    {-15, -3, -18, -3, -12, -15},   /* 59 */
    {0, -30, -27, -24, -18, -18},   /* 60 */
    {0, -30, -33, -27, -24, -15},   /* 61 */
    {-3, -21, -39, -33, -6, -6},   /* 62 */
    {-3, -24, -9, -9, -9, -21},   /* 63 */
};
static const uint8_t WAVE_FLAGS[62] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 4, 8, 10, 0, 10, 1, 10, 8, 8, 8, 8, 8, 1, 8, 8, 8, 4, 1, 5, 0, 1, 1, 0, 1, 0, 0, 0, 8, 10, 10, 10, 8, 10, 10, 0, 10, 0, 0, 8, 10, 0, 0, 10, 10, 0, 8, 8, 1, 1, 0, 0};

/* the waves' levels against the sine (dB; rest.json osc3, one RMS figure per wave) and their odd / even energy ratio
 * (dB, 3 dB steps; waves_coarse.json) */
static const int8_t WAVE_LEVEL_DB10[62] = {-41, -70, -39, -60, -54, -66, -16, -41, -38, -47, -59, -22, -56, -44, -73, -38, -47, -39, -69, -53, -55, -95, -71, -53, -63, -86, -78, -59, -48, -34, -48, -36, -17, -61, -36, -63, -22, -47, -28, -46, -25, -34, -25, -36, -37, -26, -36, -34, -41, -31, -45, -43, -44, -38, -31, -31, -32, -33, -21, -33, -61, -54};
static const int8_t WAVE_ODD_EVEN_DB[62] = {9, 6, 6, 6, 0, 0, 9, 3, 0, 3, 3, 0, -6, -18, 3, -9, 12, -9, 0, -3, 0, -3, 3, 9, -3, -6, 0, 3, 12, 12, 3, 9, 9, 0, 12, 3, 6, 3, 3, -27, -39, -42, -3, -45, -42, -6, -12, 0, 0, -3, -27, 0, 0, -39, -18, -3, -3, -3, 15, 21, 3, 6};

static float hash01(uint32_t x)          /* a seeded value in [0, 1) */
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return (float)(x >> 8) * (1.f / 16777216.f);
}

void sy_waves_harm(int id, float *amp, float *phs)   /* the designed wave id's harmonics: amp[1..64] at the
                                         * wave's level (as its table holds it), phs[1..64] (cycles: sin 2 pi (n t + phs)) */
{
    int band, n;
    float tot = 0.f, scale;
    const int8_t *bd = WAVE_BANDS[id - 2];
    int fl = WAVE_FLAGS[id - 2];
    for (n = 0; n <= 64; n++) amp[n] = 0.f;
    for (band = 0; band < 6; band++) {
        int lo = 1 << band, hi = band == 5 ? 64 : (2 << band) - 1;
        float e = FDB2LIN((float)bd[band]), sum = 0.f;     /* energy 10^(dB/10) */
        e = e * e;
        for (n = lo; n <= hi; n++) {
            float t = (fl & 4) ? 2.f : (fl & 8) ? 0.f : 1.f, wgt;
            wgt = FEXP2(-t * FLOG2((float)n));                    /* n^-tilt (power) */
            if (fl & 8) wgt *= 0.5f + hash01((uint32_t)(id * 131 + n));
            if ((fl & 1) && !(n & 1)) wgt *= 0.1f;
            if ((fl & 2) && (n & 1)) wgt *= 0.1f;
            amp[n] = wgt;
            sum += wgt;
        }
        for (n = lo; n <= hi; n++) amp[n] = sy_sqrt(amp[n] / sum * e * 2.f);   /* a^2/2 = the share */
    }
    {                                   /* the odd / even energy ratio as measured */
        float eo = 0.f, ee = 0.f, r = FDB2LIN((float)WAVE_ODD_EVEN_DB[id - 2]), t, so, se;
        r *= r;
        for (n = 1; n <= 64; n++) { if (n & 1) eo += amp[n] * amp[n]; else ee += amp[n] * amp[n]; }
        t = eo + ee;
        so = eo > 0.f ? sy_sqrt(t * r / (1.f + r) / eo) : 1.f;
        se = ee > 0.f ? sy_sqrt(t / (1.f + r) / ee) : 1.f;
        for (n = 1; n <= 64; n++) amp[n] *= (n & 1) ? so : se;
    }
    amp[0] = phs[0] = 0.f;
    for (n = 1; n <= 64; n++) { tot += 0.5f * amp[n] * amp[n]; phs[n] = n == 1 ? 0.f : hash01((uint32_t)(id * 7919 + n * 104729)); }
    scale = sy_sqrt(0.5f / tot) * FDB2LIN((float)WAVE_LEVEL_DB10[id - 2] * 0.1f);   /* (its RMS: the measured level) */
    for (n = 1; n <= 64; n++) amp[n] *= scale;
}

static float half2f(uint32_t h)          /* an IEEE half float (no infinities or NaNs expected: read as large) */
{
    const int e = (int)(h >> 10 & 31u);
    float v = (float)(e ? (h & 1023u) | 1024u : h & 1023u);
    int x = e ? e - 25 : -24;
    while (x > 0) { v *= 2.f; x--; }
    while (x < 0) { v *= 0.5f; x++; }
    return (h & 0x8000u) ? -v : v;
}

void sy_waves_build(sy_wtab_t *w, int id)
{
    float amp[65], phs[65], tot = 0.f, scale, pk = 0.f;
    int n, lvl, i;
    const uint8_t *u = sy_user_wave ? sy_user_wave(id) : 0;
    if (u) {                            /* a user wave: its harmonics; its level as the factory tables' (their peak at
                                         * full scale, as a sine's), then the record's trim */
        const uint8_t *h = u + SY_UW_HARM;
        for (n = 1; n <= 64; n++, h += 4) {
            amp[n] = half2f((uint32_t)h[0] | (uint32_t)h[1] << 8);
            if (!(amp[n] > 0.f) || amp[n] > 60000.f) amp[n] = 0.f;   /* (negative, NaN, infinite: none) */
            phs[n] = (float)((uint32_t)h[2] | (uint32_t)h[3] << 8) * (1.f / 65536.f);
            tot += amp[n];
        }
        if (!(tot > 0.f)) { amp[1] = 1.f; phs[1] = 0.f; }   /* (all silent: a sine) */
        scale = 1.f;
    } else {
        sy_waves_harm(id, amp, phs);
        scale = 1.f;
    }
    for (lvl = 0; lvl < SY_WAVE_LEVELS; lvl++) {
        int top = 64 >> lvl;
        for (i = 0; i <= SY_WAVE_N; i++) {
            float t = (float)i / SY_WAVE_N, y = 0.f;
            for (n = 1; n <= top; n++) y += amp[n] * FSIN2PI((float)n * t + phs[n]);
            w->tab[lvl][i] = y * scale;
            if (u && !lvl && sy_absf(y) > pk) pk = sy_absf(y);
        }
    }
    if (u) {                            /* (the user wave's peak: level 0's, all levels by the same) */
        scale = FDB2LIN((float)(int8_t)u[SY_UW_TRIM] * 0.1f) / (pk > 1e-9f ? pk : 1.f);
        for (lvl = 0; lvl < SY_WAVE_LEVELS; lvl++)
            for (i = 0; i <= SY_WAVE_N; i++) w->tab[lvl][i] *= scale;
    }
}

/* the sub oscillator's two shapes: odd harmonics 1-11, 1/n and 1/n^2 (osc.json sub: the reference's lines match
 * these up to the 11th; the lab's analysis itself rolls them off); unit fundamental */
