/* SPDX-License-Identifier: GPL-3.0-only */
/* One voice, one block, per sample: oscillators (the shape morph, pulse width, waves, sync, FM), sub, noise, ring
 * modulator, filter 1 (digital or analog) into the saturation stage, filter 2 and the routing, the amp and pan.
 * Every value that is constant over the block comes in sy_vblk_t (synth.c); gains and the cutoff are interpolated
 * from the last block's values to this block's. */
#include "sy_int.h"
/* the voice's hot code in RAM on the FM-1 (SY_HOT_RAM: out of the flash cache's way); elsewhere nothing */
#ifdef SY_HOT_RAM
#define SY_HOT __attribute__((section(".ram_text")))
#else
#define SY_HOT
#endif

SY_WORK_ATTR float sy_sat_lut[SAT_N + 1];
#define SUB_TRI_G 2.57f                 /* the triangle's level against the square's (osc.json sub: +7.3 dB) */
/* (BP0, the band pass weight, in sy_int.h) */

/* ---- the saturation curves of types 1-4: y = L u / (1 + |u|^p)^(1/p), u = k x / L, separately for each half-wave
 * (shared k). Fitted to the full static curves of sat.json at drive 64, 96 and 127 (within 0.75 % of full scale);
 * between them the parameters move geometrically (L, k) and linearly (p) */
static const float SAT_P[4][3][5] = {   /* Lpos, Lneg, k, ppos, pneg at drive 64, 96, 127 */
    {{0.7384f, 0.3799f, 1.2205f, 1.7067f, 1.6368f}, {0.3765f, 0.1909f, 1.7393f, 1.5124f, 1.4677f},
     {0.2145f, 0.1080f, 2.8021f, 1.3803f, 1.3539f}},
    {{1.1866f, 0.1640f, 1.1910f, 1.8294f, 1.5599f}, {0.6172f, 0.0811f, 1.6697f, 1.6194f, 1.4457f},
     {0.3772f, 0.0482f, 2.8312f, 1.4495f, 1.3535f}},
    {{0.4622f, 0.1194f, 1.7618f, 1.5640f, 1.4329f}, {0.2789f, 0.0707f, 3.0813f, 1.4082f, 1.3336f},
     {0.2156f, 0.0542f, 6.8470f, 1.3093f, 1.2775f}},
    {{0.2063f, 0.1045f, 2.9226f, 1.4889f, 1.3792f}, {0.1568f, 0.0788f, 6.6914f, 1.3526f, 1.2859f},
     {0.1320f, 0.0660f, 19.829f, 1.1699f, 1.1683f}}};

static inline __attribute__((always_inline)) float softc(float x, float L, float k, float p)
{
    float u = k * x / L, a = sy_absf(u);
    if (a < 1e-9f) return k * x;
    return L * u * FEXP2(-FLOG2(1.f + FEXP2(p * FLOG2(a))) / p);
}

#ifdef HYB_MULTI
#define sy_sat_lut (s->sat_lut)         /* (two parts: the part's own curve) */
#endif
void sy_sat_build(sy_synth *s, int type, float d)   /* d = drive 0..1 */
{
    int i, seg = d > 0.508f;
    float q[5], t = seg ? (d - 0.508f) / 0.492f : d / 0.508f;
    const float *P0 = SAT_P[type - 1][seg], *P1 = SAT_P[type - 1][seg + 1];
    (void)s;
    for (i = 0; i < 5; i++)
        q[i] = i < 3 ? FEXP2(FLOG2(P0[i]) + t * (FLOG2(P1[i]) - FLOG2(P0[i]))) : P0[i] + t * (P1[i] - P0[i]);
    for (i = 0; i <= SAT_N; i++) {
        float x = ((float)i / SAT_N * 2.f - 1.f) * SAT_RANGE;
        sy_sat_lut[i] = SAT_TRIM * (x >= 0.f ? softc(x, q[0], q[2], q[3]) : -softc(-x, q[1], q[2], q[4]));
    }
}

#ifdef HYB_MULTI
#undef sy_sat_lut
#define sy_sat_lut lut                  /* (two parts: the voice's part's curve, sy_vblk_t) */
#define sat_curve(x) sat_curve_l(b->sat_lut, x)
static inline __attribute__((always_inline)) float sat_curve_l(const float *lut, float x)
#else
static inline __attribute__((always_inline)) float sat_curve(float x)
#endif
{
    float t = (x * (1.f / SAT_RANGE) + 1.f) * (SAT_N * 0.5f), f;
    int i;
    if (t <= 0.f) return sy_sat_lut[0];
    if (t >= (float)SAT_N) return sy_sat_lut[SAT_N];
    i = (int)t;
    f = t - (float)i;
    return sy_sat_lut[i] + f * (sy_sat_lut[i + 1] - sy_sat_lut[i]);
}
#ifdef HYB_MULTI
#undef sy_sat_lut
#endif

/* ---- oscillators */
#include "sy_hyb_blepx.h"
#ifdef LADSAT_HDR
#include LADSAT_HDR
#else
#include "sy_hyb_ladsat.h"
#endif
#ifndef LAD_STS_ON
#define LAD_STS_ON 1
#endif
#ifndef SAT_VPOST_MIN
#define SAT_VPOST_MIN 5
#endif
#ifndef SAT_VPRE
#define SAT_VPRE 0.9623f
#endif
#ifndef LAD_STS
#define LAD_STS 1.2f                    /* the three later stages saturate too, by the input's curve scaled by
                                         * this (read from a cutoff sweep with drive: above the corner the reference's
                                         * harmonics fall less than a linear cascade lets them; with linear stages the
                                         * fit's cost was 2.3 dB mean / 9 max, with these 1.3 / 5) */
#endif

#if LAD_STS_ON
#include "sy_hyb_ladst.h"
#ifndef LAD_NZ_MODE
#define LAD_NZ_MODE 0
#endif
#ifndef LAD_NZ_A
#define LAD_NZ_A 2.3e-4f
#endif
static inline __attribute__((always_inline)) float lad_st(float v, float sk, float ss)  /* the stages' own saturation: S(v / s) s (sk = LADST_K / s,
                                         * ss = s) */
{
    float w = sy_absf(v) * sk, x;
    int q = (int)w;
    x = q >= LADST_N ? LADST[LADST_N] : LADST[q] + (w - (float)q) * (LADST[q + 1] - LADST[q]);
    x *= ss;
    return v < 0.f ? -x : x;
}
#endif
#ifndef LAD_NZ_POST
#define LAD_NZ_POST 1                   /* in the analog modes the noise enters the loop past its input stage
                                         * (read with the noise alone: the resonance's peak and the level within ~1 dB;
                                         * through the input stage the peak read 8-12 dB low, the level 2.5 dB) */
#endif
#ifndef LAD_NZ_G
#define LAD_NZ_G 0.97f                  /* (read with the stage saturation: within ~1 dB from cutoff 40 to 127) */
#endif
#ifndef SYNC_HOLD
#define SYNC_HOLD 0.78f
#endif
#ifndef NFM_K
#define NFM_K 0.47f
#endif
#include "sy_hyb_click.h"
#ifndef BLEPX_SIGN
#define BLEPX_SIGN -2.f
#endif
static inline __attribute__((always_inline)) float blepx_at(float tau)        /* tau: samples from the jump, -1.5 .. 8 */
{
    float x = (tau + 1.5f) * 16.f, f;
    int i;
    if (x <= 0.f || x >= (float)(BLEPX_N - 1)) return 0.f;
    i = (int)x;
    f = x - (float)i;
    return BLEPX[i] + f * (BLEPX[i + 1] - BLEPX[i]);
}
static inline __attribute__((always_inline)) float blep_in(float t, float dt, float idt)  /* the 2-point polyBLEP residual of a
                                         * unit (+2) jump at t = 0; idt = 1 / dt (the same quotient, once a block) */
{
    float r = 0.f;
#ifdef HYB_FAST_VOICE                   /* (INAUDIBLE: by the reciprocal, 1 ulp at most) */
    if (t < dt) { float u = t * idt; r = u + u - u * u - 1.f; }
    else if (t > 1.f - dt) { float u = (t - 1.f) * idt; r = u * u + u + u + 1.f; }
#else
    if (t < dt) { float u = t / dt; r = u + u - u * u - 1.f; }
    else if (t > 1.f - dt) { float u = (t - 1.f) / dt; r = u * u + u + u + 1.f; }
#endif
    if (t < 8.f * dt || t > 1.f - 1.5f * dt) {    /* the reference's step shape (sy_hyb_blepx.h) */
        float c = 0.f, tau;
        int k;
        if (t > 1.f - 1.5f * dt) c += blepx_at((t - 1.f) * idt);
        for (k = 0, tau = t * idt; tau < 8.f && k < 16; k++, tau += idt) c += blepx_at(tau);
        r += BLEPX_SIGN * c;
    }
    return r;
}
/* (out of line where it is called from many places: the code room; inline in osc_run's saw and pulse loops, the
 * common edges: no call frame per edge) */
static SY_HOT __attribute__((noinline)) float blep(float t, float dt, float idt) { return blep_in(t, dt, idt); }


#define U2F 2.3283064e-10f              /* 2^-32 */
#ifdef HYB_FAST_VOICE
/* INAUDIBLE (HYB_FAST_VOICE, off by default: not bit-exact): the sine from a table of 2048 points, linearly
 * interpolated (error 1.2e-6 peak, -121 dB RMS re the sine), the phase exact (its top 11 bits the point, the next 21 the
 * fraction); reciprocals for divisors constant over the block */
static inline __attribute__((always_inline)) float sin_ph(uint32_t ph)     /* sin(2 pi ph / 2^32) */
{
    uint32_t i = ph >> 21;
    float f = (float)(ph & 0x1FFFFFu) * (1.f / 2097152.f), a = SY_SIN_TAB[i];
    return a + f * (SY_SIN_TAB[i + 1] - a);
}
static inline __attribute__((always_inline)) float sin_cyc(float t)        /* sin(2 pi t); ASSUMPTION: |t| < 2^19
                                         * (saturation 6's argument), so t * 2048 fits an int with its fraction */
{
    float x = t * (float)SY_SIN_N, f, a;
    int n = sy_floori(x), i = n & (SY_SIN_N - 1);
    f = x - (float)n;
    a = SY_SIN_TAB[i];
    return a + f * (SY_SIN_TAB[i + 1] - a);
}
#define SIN_PH(ph, t) sin_ph(ph)
#define SIN_PHU(ph, t) sin_ph(ph)
#define SIN_CYC(t) sin_cyc(t)
#else
#define SIN_PH(ph, t) sy_sin2pi(t)
#define SIN_CYC(t) sy_sin2pi(t)
/* sy_sin2pi for t in [0, 1] (a phase's float): its floor(t + 0.5) is 0 or 1, taken by a test (the same value) */
static inline __attribute__((always_inline)) float sin2pi_u(float t)
{
    float x, x2;
    t -= t + 0.5f >= 1.f ? 1.f : 0.f;          /* -0.5 .. 0.5 */
    if (t > 0.25f) t = 0.5f - t;
    else if (t < -0.25f) t = -0.5f - t;
    x = t * SY_2PI;
    x2 = x * x;
    return x * (1.f + x2 * (-0.166666667f + x2 * (0.00833333333f + x2 * (-0.000198412698f
        + x2 * (2.75573192e-6f - x2 * 2.50521084e-8f)))));
}
#define SIN_PHU(ph, t) sin2pi_u(t)
#endif
static const float sy_zero_blk[SY_BLOCK];   /* a block of +0: what a source that is off would have written */
/* blep() of a phase ph (t its float): 0 without the call where it is surely 0 (osc_setup's window) */
#define BLEP_PH(ph, t, o) ((uint32_t)((ph) - (o)->blo) < (o)->bspan ? 0.f : blep(t, (o)->dtw, (o)->idtw))
/* (BLEP_PHI: blep inlined into osc_run's saw and pulse loops was tried in 0.26: fewer instructions per call, but the
 * loop spilled and osc_run ran 28 % more instructions on bank patch +30; back to the call) */
#define BLEP_PHI(ph, t, o) BLEP_PH(ph, t, o)

/* CLEANER (P3_SPAN): the simple oscillator's saws as runs. Between the jumps' windows the saw is
 * naive (blep() is 0 there: osc_setup's window, BLEP_PH); a run's length comes from the phase once per run (a multiply
 * by the block's reciprocal of the step, rounded down: never too long), so the quiet samples need no window test and
 * no call; in a window each sample is tested as before (P3_BLEPW: blep's terms with the block's thresholds). The same
 * values as the per-sample loops (bit-identical). -DP3_SPAN=0: the old loops. */
#ifndef P3_SPAN
#define P3_SPAN 1
#endif
/* (P3_RAMP: a quiet run as the line through its first sample, y0 + slope j: no phase conversion a sample; within
 * ~1e-7 of the phase's own value. Not bit-identical, and any last-bit change of the plain oscillators shows in a patch
 * with a strong narrow low EQ (dev030: the mid band at 28 Hz, +58 dB on the voices' difference): -74 dBFS against the
 * per-sample phases, the slope's exactness does not matter (wave 2: tried as two floats, and the per-sample
 * expression 1 - 2t as (1 - t) - t alone: the same -74). 0 by default in wave 2,
 * 1 again in wave 3 (the user's -92 dBFS bound with dev030 as the documented exception, section 11); -DP3_RAMP=0:
 * the per-sample phases (and P3_EV off) */
#ifndef P3_EV
#define P3_EV P3_RAMP                   /* (needs P3_SPAN and lines: P3_RAMP) */
#endif
#ifndef P3_RAMPS
#define P3_RAMPS P3_RAMP                /* (the lines of saw_span: osc_run's saws and pulses) */
#endif
#ifndef P3_RAMPY
#define P3_RAMPY P3_SPAN                /* (the lines of osc_sync_span: osc 2 synced, its saws alone; on by default: the
                                         * broad null sweep against 1.18C31 shows nothing from them, cpu-baseline.md 10) */
#endif
#ifndef P3_RAMPQ
#define P3_RAMPQ P3_RAMP                /* (the sub's square held over its quiet runs) */
#endif
#ifndef P3_PL6
#define P3_PL6 1
#endif
#ifndef P3_O2S
#define P3_O2S 1                        /* sub_ring takes osc 2's samples from w->o2s when the tick path left them */
#endif
#ifndef P3_MIX
#define P3_MIX 1                        /* the plain path's mix with the ring and no osc 3 on its own loop, RING_SUB in it */
#endif
#ifndef P3_SIN
#define P3_SIN 1
#endif
#ifndef P3_O2P
#define P3_O2P 1
#endif
#ifndef P3_RAMP
#define P3_RAMP 1                       /* (wave 2: off by default, see above; wave 3: on again under the user's
                                         * relaxed null bound, -92 dBFS with documented exceptions: dev030's 28 Hz EQ
                                         * band, cpu-plan.md section 6 and 9; 0: the per-sample phases; needs P3_SPAN) */
#endif
/* (wave 2: the factory patch A010 on the target; each bit-identical, -DNAME=0 the wave-1 code)
 * P3_HIFB: osc_run's saws whose window is not one sample (a moving pulse width moves the increment off the step,
 * PW_PH) through saw_span, not the per-sample osc_hi, when P3_EV is on;
 * P3_PH2S: osc_ph2's sync with the triangle FM (modes 0, 1) on a loop of its own (the state in registers, the FM's
 * floor skipped where the offset lies in [0, 1): c - 0 = c);
 * P3_MIX2: the plain path's mix with osc 3 or the ring in two passes (the sum, then the low pass in place), RING_SUB's
 * factor on the sub in the first (sub_ring's pass gone);
 * P3_NZ: the noise per colour on a loop of its own into xn, then added where it goes (nd[i] += g n as before) */
#ifndef P3_HIFB
#define P3_HIFB 1
#endif
#ifndef P3_PH2S
#define P3_PH2S 1
#endif
#ifndef P3_MIX2
#define P3_MIX2 1
#endif
#ifndef P3_NZ
#define P3_NZ 1
#endif
#ifndef P3_EVTK
#define P3_EVTK P3_SPAN                 /* (P3_EV's osc_saw2 on the tick path's own plain renders only: the sub, and osc 2
                                         * for RING_SUB's factor (sub_ring); off the tick path not: P3_EV's sums in
                                         * another order fail dev030's null, cpu-baseline.md 10) */
#endif
#ifndef P3_SYF
#define P3_SYF 1                        /* (vr_osc's sync pass after osc_ph2: the saws alone or a sine and the saws on
                                         * loops of their own, blep through the block's constants as osc_sync_span:
                                         * osc_at_sync's terms, bit-identical) */
#endif
#ifndef P3_LPN
#define P3_LPN 1                        /* (wave 3) the plain path's mix with osc 3 or the ring: the low pass and the
                                         * noise in one pass after mix_sum, where nothing comes between them (no dip, no
                                         * start transient, no split, the noise into the mix): lp + g n as before, the
                                         * same expressions (bit-identical); vr_osc's noise stage then skipped */
#endif
#ifndef P3_RGK
#define P3_RGK 1                        /* (wave 3) mix_sum's ring term as (o1 x (rg r1 r2)) x o2 (the constant's product
                                         * once: two multiplies a sample less, the constants in registers; the last bits
                                         * of the ring's part change: null at -150 dBFS) */
#endif
#ifndef P3_SYP
#define P3_SYP 1                        /* (wave 3) P3_SYF's sync pass as a function of its own (sync_pass: the phases'
                                         * blocks and osc 2's constants in registers, one loop per shape) */
#endif
#ifndef P3_SYQ
#define P3_SYQ P3_SYP                   /* (wave 3) sync_pass's sine by a shorter polynomial in t (sin2pi_q, error 3e-9:
                                         * two multiplies and an add less); 0: sin2pi_u (bit-identical). (The saws' gains
                                         * as w (1 - c) and w c were tried: dev053's two resonant band passes and drive
                                         * show that last bit at -88.8 dBFS for one multiply a sample: not taken) */
#endif
#ifndef P3_SUBT
#define P3_SUBT 1                       /* (wave 3) the sub's triangle as lines between its corners (the corners' samples
                                         * as before: within ~1e-7 of the per-sample values) */
#endif
#ifndef P3_SUBQ
#define P3_SUBQ 1                       /* (the sub's square over its quiet runs without the line: an equal expression) */
#endif
#if P3_SPAN
typedef struct { float dt, idt, d8, e15, e1; } blk_t;   /* blep's constants: dt, 1 / dt, 8 dt, 1 - 1.5 dt, 1 - dt */
static inline __attribute__((always_inline)) void blk_set(blk_t *k, float dt, float idt)
{
    k->dt = dt; k->idt = idt; k->d8 = 8.f * dt; k->e15 = 1.f - 1.5f * dt; k->e1 = 1.f - dt;
}
#ifndef P3_BLEPW
#define P3_BLEPW 1                      /* (0: the window samples through blep, no code added; 1 through blepw, one copy;
                                         * 2 blep_wi inline at each window branch: ~10 % fewer instructions in the
                                         * windows, ~2.5 KB more code) */
#endif
/* blep() where the window is open (span != 0: then 1 / dt > 8.59, so a phase is either after its jump, t < 8 dt, or
 * before it, t > 1 - 1.5 dt, never both; before it the last jump's tail too while the cycle is under 9.5 samples):
 * blep_in's terms without its tests that cannot hold (bit-identical) */
static inline __attribute__((always_inline)) float blep_wi(float t, const blk_t *k)
{
    const float dt = k->dt, idt = k->idt;
    float r = 0.f, c, tau;
    if (t < k->d8) {
#ifdef HYB_FAST_VOICE
        if (t < dt) { float u = t * idt; r = u + u - u * u - 1.f; }
#else
        if (t < dt) { float u = t / dt; r = u + u - u * u - 1.f; }
#endif
        c = blepx_at(t * idt);
    } else if (t > k->e15) {
#ifdef HYB_FAST_VOICE
        if (t > k->e1) { float u = (t - 1.f) * idt; r = u * u + u + u + 1.f; }
#else
        if (t > k->e1) { float u = (t - 1.f) / dt; r = u * u + u + u + 1.f; }
#endif
        c = blepx_at((t - 1.f) * idt);
        tau = t * idt;
        if (tau < 8.f) c += blepx_at(tau);
    } else return 0.f;
    return r + BLEPX_SIGN * c;
}
#if P3_BLEPW == 1
static SY_HOT __attribute__((noinline)) float blepw(float t, const blk_t *k) { return blep_wi(t, k); }
#define BLEPW(t, k) blepw(t, k)
#elif P3_BLEPW
#define BLEPW(t, k) blep_wi(t, k)
#else
#define BLEPW(t, k) blep(t, (k)->dt, (k)->idt)
#endif
/* the quiet samples ahead of a phase r = ph - blo inside the window's complement (r < span): r + j inc < span for j
 * below the result; rcp = (2^32 - 1) / inc, so the product rounds down (one sample short at worst) */
/* the high word of a 32 x 32-bit product: out of line, the product used at once (the device: a 64-bit multiply
 * inlined beside loads now and then leaves a register a bit off, fm1-base bsp/docs/CPU.md; the build's check) */
static SY_HOT __attribute__((noinline)) uint32_t mulhi32(uint32_t a, uint32_t b)
{
    return (uint32_t)(((uint64_t)a * b) >> 32);
}
static inline __attribute__((always_inline)) uint32_t qrun(uint32_t r, uint32_t span, uint32_t rcp)
{
    return mulhi32(span - 1u - r, rcp) + 1u;
}
#if P3_EV || P3_EVTK
/* (P3_EV) two saws as lines and jumps: out[i] = A S(t1) + C S(t2), S(t) = 1 - 2t + blep(t), t1 the phase ph + (i + 1) inc,
 * t2 the phase d on (HALF: as the square sub reads it, t1 + 0.5 in floats). The naive saws as lines from each stretch's
 * first sample to the next wrap of either (as P3_RAMP), then each jump's residual added to the samples of its window
 * (those within 8 samples after it or 1.5 before: blep_in's terms per sample, the phase read as before, without a
 * call and the window tests a sample). The window needs span != 0 (1 / dt > 8.59). Not bit-identical: the sums in
 * another order, the lines. */
static SY_HOT __attribute__((noinline)) float osc_evt(uint32_t f, uint32_t d, int half)   /* a saw's phase as read */
{
    float u;
    if (!half) return (float)f * U2F;
    u = (float)(f - d) * U2F + 0.5f;
    return u >= 1.f ? u - 1.f : u;
}
static SY_HOT __attribute__((noinline)) void osc_saw2(float *out, uint32_t ph, uint32_t inc, uint32_t d, float A, float C,
                                                    const blk_t *k, int half, int stride)
{
    const float sl = -2.f * (float)inc * U2F * (A + C), dt = k->dt, idt = k->idt;
    const int ns = C != 0.f ? 2 : 1;
    uint32_t p = ph;
    int i = 0, s;
    while (i < SY_BLOCK) {                              /* the naive saws: a line to the next wrap of either */
        const uint32_t q = p + inc;
        uint32_t m = (0xFFFFFFFFu - q) / inc + 1u, e = (uint32_t)(SY_BLOCK - i);
        float y0 = A * (1.f - 2.f * ((float)q * U2F)), jf = 1.f;
        if (ns > 1) {
            const uint32_t m2 = (0xFFFFFFFFu - (q + d)) / inc + 1u;
            if (m2 < m) m = m2;
            y0 += C * (1.f - 2.f * osc_evt(q + d, d, half));
        }
        if (m > e) m = e;
        e = (uint32_t)i + m;
        out[i++] = y0;
        for (; i < (int)e; i++, jf += 1.f) out[i] = y0 + sl * jf;
        p += inc * m;
    }
    for (s = 0; s < ns; s++) {                          /* the jumps, saw by saw */
        const float g = s ? C : A;
        const int hf = s && half;
        const uint32_t f0 = ph + inc + (s ? d : 0u), n0 = f0 / inc;
        int j0 = -(int)n0;                              /* the first sample after the last wrap up to sample 0 */
        uint32_t fj = f0 - n0 * inc;                    /* (its phase: below inc) */
        while (j0 < SY_BLOCK + 3) {
            int m = j0 > 3 ? -3 : -j0;                  /* (the samples from 1.5 before the jump to 8 after it) */
            for (; m < 0 && j0 + m < SY_BLOCK; m++) {  /* before it: t > 1 - 1.5 dt (the phase read as before) */
                const float t = osc_evt(fj - (uint32_t)(-m) * inc, d, hf);
                float r = 0.f;
                if (!(t > k->e15)) continue;
                if (t > k->e1) { float u = (t - 1.f) / dt; r = u * u + u + u + 1.f; }
                out[j0 + m] += g * (r + BLEPX_SIGN * blepx_at((t - 1.f) * idt));
            }
            if (j0 + m < SY_BLOCK) {                    /* after it: the table read at the first sample's fraction,
                                         * STRIDE points (16 a sample: 1 / dt a cycle) on per sample, while t idt < 8 */
                const float t0 = osc_evt(fj, d, hf), x0 = (t0 * idt + 1.5f) * 16.f;
                const int i0 = (int)x0;
                const float f = x0 - (float)i0;
                int ix = i0 + stride * m;
                for (; j0 + m < SY_BLOCK && ix < BLEPX_N - 1; m++, ix += stride) {
                    float r = BLEPX_SIGN * (BLEPX[ix] + f * (BLEPX[ix + 1] - BLEPX[ix]));
                    if (ix < 40) {                      /* (t < dt: the 2-point polyBLEP, its phase read as before) */
                        const float t = osc_evt(fj + (uint32_t)m * inc, d, hf);
                        if (t < dt) { float u = t / dt; r += u + u - u * u - 1.f; }
                    }
                    out[j0 + m] += g * r;
                }
            }
            m = (int)((0xFFFFFFFFu - fj) / inc) + 1;
            j0 += m;
            fj += (uint32_t)m * inc;
        }
    }
}
#endif
/* osc_run's saw (PU 0: ws = 0, c = 0) and pulse (PU 1: ws = 0, the second saw at ph + d): its loops' expressions
 * (PU a block constant, not a compile-time one: one copy of the code) */
static inline __attribute__((always_inline)) void saw_span(const sy_osc_t *o, uint32_t ph, float *out, const int PU)
{
    const uint32_t inc = o->inc, blo = o->blo, span = o->bspan, d = o->d, rcp = 0xFFFFFFFFu / inc;
    const float w = o->wsaw, c = o->c;
#if P3_RAMPS
    const float sl = -2.f * (float)inc * U2F * (PU ? w * (1.f - c) : w);    /* the naive pulse's slope a sample */
#endif
    blk_t k;
    int i = 0;
    blk_set(&k, o->dtw, o->idtw);
    while (i < SY_BLOCK) {                              /* a sample as before; when quiet, the run after it */
        float t, s1, y0;
        uint32_t r1, r2;
        ph += inc;
        r1 = ph - blo; r2 = ph + d - blo;
        t = (float)ph * U2F;
        s1 = 1.f - 2.f * t + (r1 < span ? 0.f : BLEPW(t, &k));
        if (PU) { const float t2 = (float)(ph + d) * U2F; s1 -= c * (1.f - 2.f * t2 + (r2 < span ? 0.f : BLEPW(t2, &k))); }
        out[i++] = y0 = w * s1;
        if (r1 < span && (!PU || r2 < span)) {
            uint32_t m = qrun(r1, span, rcp) - 1u, e = (uint32_t)(SY_BLOCK - i);
            if (PU) { const uint32_t m2 = qrun(r2, span, rcp) - 1u; if (m2 < m) m = m2; }
            if (m > e) m = e;
            e = (uint32_t)i + m;
#if P3_RAMPS
            {                                           /* the line through the sample */
                float jf = 1.f;
                for (; i < (int)e; i++, jf += 1.f) out[i] = y0 + sl * jf;
                ph += inc * m;
            }
#else
            for (; i < (int)e; i++) {                   /* the naive saws (2t as ph 2^-31: the same value, both
                                         * products exact) */
                ph += inc;
                s1 = 1.f - (float)ph * 4.65661287e-10f;
                if (PU) s1 -= c * (1.f - (float)(ph + d) * 4.65661287e-10f);
                out[i] = w * s1;
            }
#endif
        }
    }
}
#endif


static inline __attribute__((always_inline)) uint32_t cyc2ph(float c)  /* a phase offset in cycles -> a phase step, wrapped */
{
    c -= (float)sy_floori(c);
    return (uint32_t)(c * 4294967040.f);
}
static inline __attribute__((always_inline)) uint32_t rad2ph(float r) { return cyc2ph(r * 0.159154943f); }
/* a triangle from osc 1's phase at sample i of the block (ph1: the phase before the block): +1 at the wrap, -1
 * half way */
static inline __attribute__((always_inline)) float tri1(uint32_t ph1, uint32_t inc, int i)
{
    float u = (float)(ph1 + inc * (uint32_t)i) * U2F;          /* (one sample late, as osc 1's wave in mode 2) */
    return u < 0.5f ? 1.f - 4.f * u : 4.f * u - 3.f;
}

static inline __attribute__((always_inline)) float wave_at(const sy_osc_t *o, uint32_t ph)
{
    float t = (float)ph * U2F;
    switch (o->wave) {
    case 0: return SIN_PH(ph, t);
    case 1: {                           /* triangle, its fundamental 1, in phase with the sine */
        float y = t < 0.25f ? 4.f * t : t < 0.75f ? 2.f - 4.f * t : 4.f * t - 4.f;
        return y * 1.23370055f;
    }
    case 64: {                          /* the narrow pulse (duty 0.02), RMS as the sine */
        uint32_t p2 = ph + 85899346u;   /* 0.02 cycle */
        float t2 = (float)p2 * U2F;
        float y = (1.f - 2.f * t + BLEP_PH(ph, t, o)) - (1.f - 2.f * t2 + BLEP_PH(p2, t2, o));
        return y * 0.803f;              /* RMS 10 dB under the sine (rest.json osc3 mode 68) */
    }
    default: {
        float x = t * (float)SY_WAVE_N, f;
        int i = (int)x;
        f = x - (float)i;
        return o->wt[i] + f * (o->wt[i + 1] - o->wt[i]);
    }
    }
}

/* the morph: ws * wave + wsaw * (S(t) - c S(t + d)), S the band-limited falling saw with a unit fundamental */
static inline __attribute__((always_inline)) float osc_at(const sy_osc_t *o, uint32_t ph)
{
    float y = 0.f;
    if (o->ws != 0.f) y = o->ws * wave_at(o, ph);
    if (o->wsaw != 0.f) {
        float t = (float)ph * U2F;
        float s1 = 1.f - 2.f * t + BLEP_PH(ph, t, o);
        if (o->c != 0.f) {
            float t2 = (float)(ph + o->d) * U2F;
            s1 -= o->c * (1.f - 2.f * t2 + BLEP_PH(ph + o->d, t2, o));
        }
        y += o->wsaw * s1;
    }
    return y;
}

/* osc 2 under sync. After each reset the saw part lags by SYNC_HOLD samples' phase until the next reset.
 * Read: the master's frequency appears at an exact ratio, rising 6 dB per octave. It reads -27 dB re the strongest
 * line for a saw at key 72, about 8 dB lower for shape 96, and none for a pulse or a sine. The pulse (the saws'
 * difference) and the sine keep the exact phase. */
static inline __attribute__((always_inline)) float osc_at_sync(const sy_osc_t *o, uint32_t ph, uint32_t phl)
{
    float y = 0.f;
    if (o->ws != 0.f) y = o->ws * wave_at(o, ph);
    if (o->wsaw != 0.f) {
        float t = (float)ph * U2F, tl = (float)phl * U2F;
        float s1 = 1.f - 2.f * t + BLEP_PH(ph, t, o), sl = 1.f - 2.f * tl + BLEP_PH(phl, tl, o);
        float s = (1.f - o->c) * sl;
        if (o->c != 0.f) {              /* (s1 computed inside this branch was tried in 0.26: the same values, but no
                                         * bank patch has c = 0 under sync, and vr_osc's registers moved: +3 % on +18) */
            float t2 = (float)(ph + o->d) * U2F;
            s += o->c * (s1 - (1.f - 2.f * t2 + BLEP_PH(ph + o->d, t2, o)));
        }
        y += o->wsaw * s;
    }
    return y;
}

/* the n-th root of r, exact enough that its n-th power lands on r (the approximate exp2/log2 alone leave a
 * step of ~0.1 % at every block's start in a decay) */
static SY_HOT float qroot(float r, int n)
{
    float q = sy_exp2(sy_log2(r) / (float)n), p = 1.f, b = q;
    int k = n;
    while (k) { if (k & 1) p *= b; b *= b; k >>= 1; }
    return q * (1.f + (r / p - 1.f) / (float)n);
}

#include "sy_cln_osc.inc"

/* an oscillator over the block (the phase advanced before each sample), specialised on its shape's parts */
#if P3_SPAN
/* (P3_SPAN) osc_run's saws at a high pitch (the window everywhere): the pulse's loop (with c = 0 the saw's values but
 * the sign of a zero) */
static SY_HOT __attribute__((noinline)) void osc_hi(const sy_osc_t *o, uint32_t ph, float *out)   /* (RAM: not rare,
                                         * the factory patch A010 runs it ~43 k a block in the emulator) */
{
    const sy_osc_t q = *o;
    int i;
    for (i = 0; i < SY_BLOCK; i++) {
        float t, t2, s1;
        ph += q.inc;
        t = (float)ph * U2F;
        s1 = 1.f - 2.f * t + BLEP_PHI(ph, t, &q);
        t2 = (float)(ph + q.d) * U2F;
        s1 -= q.c * (1.f - 2.f * t2 + BLEP_PHI(ph + q.d, t2, &q));
        out[i] = q.wsaw * s1;
    }
}
/* (P3_SPAN) osc_run's general loop (a wave other than the sine): a function of its own, the shape's facts told to the
 * compiler as the old chain of branches told it (osc_at's tests of them off) */
static SY_HOT __attribute__((noinline)) void osc_gen(const sy_osc_t *o, uint32_t ph, float *out)
{
    const sy_osc_t q = *o;
    int i;
    for (i = 0; i < SY_BLOCK; i++) {
        ph += q.inc;
        if (q.ws == 0.f) __builtin_unreachable();
#ifndef HYB_FAST_VOICE
        if (q.wave == 0) __builtin_unreachable();
#endif
        out[i] = osc_at(&q, ph);
    }
}
/* (P3_SPAN) osc_run: the saws (saw_span, or at a high pitch the pulse's loop), then the sine added (P3_SIN: in runs of
 * 16 by the phasor, the exact point at each run's start), else the general loop. The same values as the old loops
 * with P3_RAMP=0 (the sine's sum in the other order: a + b = b + a; with c = 0 at a high pitch a zero's sign) */
static SY_HOT void osc_run(const sy_osc_t *o, uint32_t ph, float *out, const int ev)  /* (ev: osc_saw2 allowed, P3_EVTK) */
{
    const sy_osc_t q = *o;
    int i;
#if P3_SIN && !defined(HYB_FAST_VOICE)
    if (q.ws == 0.f || q.wave == 0) {
        const int saw = q.ws == 0.f || q.wsaw != 0.f;
#else
    if (q.ws == 0.f) {
        const int saw = 1;
#endif
        if (!saw) ;
#if P3_EV || P3_EVTK
        else if ((P3_EV || ev) && q.bspan != 0u && q.inc != 0u && sy_absf((float)q.inc * U2F * q.idtw - 1.f) < 1e-5f) {   /* (a sample is
                                         * 1 / dt a cycle: osc_saw2 reads the table 16 points on per sample) */
            blk_t k;
            blk_set(&k, q.dtw, q.idtw);
            osc_saw2(out, ph, q.inc, q.d, q.wsaw, -q.wsaw * q.c, &k, 0, 16);
        }
#endif
#if !P3_EV || P3_HIFB || P3_EVTK
        else if (q.bspan != 0u && q.inc != 0u) saw_span(&q, ph, out, q.c != 0.f);   /* (one copy: the pulse's terms
                                         * behind a test of the run) */
#endif
        else osc_hi(&q, ph, out);       /* (span 0: a high pitch, every sample in a window) */
#if P3_SIN && !defined(HYB_FAST_VOICE)
        if (q.ws != 0.f) {
            const float cr = sy_cos2pi((float)q.inc * U2F), sr = sy_sin2pi((float)q.inc * U2F);
            for (i = 0; i < SY_BLOCK; i += 16) {
                float sn, cs;
                int j;
                ph += q.inc;
                {
                    const float t = (float)ph * U2F;
                    sn = sy_sin2pi(t); cs = sy_cos2pi(t);
                }
                {
                    const float y = q.ws * sn;
                    out[i] = saw ? y + out[i] : y;
                }
                if (saw) for (j = 1; j < 16; j++) {
                    const float u = cs * cr - sn * sr;
                    sn = sn * cr + cs * sr; cs = u;
                    out[i + j] = q.ws * sn + out[i + j];
                } else for (j = 1; j < 16; j++) {
                    const float u = cs * cr - sn * sr;
                    sn = sn * cr + cs * sr; cs = u;
                    out[i + j] = q.ws * sn;
                }
                ph += q.inc * 15u;
            }
        }
#endif
    }
#if !P3_SIN && !defined(HYB_FAST_VOICE)
    else if (q.wave == 0) {             /* (the sine by a phasor, as before P3_SIN) */
        const float cr = sy_cos2pi((float)q.inc * U2F), sr = sy_sin2pi((float)q.inc * U2F);
        float sn = 0.f, cs = 0.f;
        for (i = 0; i < SY_BLOCK; i++) {
            float y;
            ph += q.inc;
            if ((i & 15) == 0) { const float t = (float)ph * U2F; sn = sy_sin2pi(t); cs = sy_cos2pi(t); }
            else { const float u = cs * cr - sn * sr; sn = sn * cr + cs * sr; cs = u; }
            y = q.ws * sn;
            if (q.wsaw != 0.f) {
                float t = (float)ph * U2F;
                float s1 = 1.f - 2.f * t + BLEP_PH(ph, t, &q);
                if (q.c != 0.f) {
                    float t2 = (float)(ph + q.d) * U2F;
                    s1 -= q.c * (1.f - 2.f * t2 + BLEP_PH(ph + q.d, t2, &q));
                }
                y += q.wsaw * s1;
            }
            out[i] = y;
        }
    }
#endif
    else osc_gen(o, ph, out);
}
#else
static SY_HOT void osc_run(const sy_osc_t *o, uint32_t ph, float *out, const int ev)
{
    (void)ev;
    const sy_osc_t q = *o;
    int i;
    if (q.ws == 0.f && q.c == 0.f) {
        for (i = 0; i < SY_BLOCK; i++) {
            float t;
            ph += q.inc;
            t = (float)ph * U2F;
            out[i] = q.wsaw * (1.f - 2.f * t + BLEP_PHI(ph, t, &q));
        }
    } else if (q.ws == 0.f) {
        for (i = 0; i < SY_BLOCK; i++) {
            float t, t2, s1;
            ph += q.inc;
            t = (float)ph * U2F;
            s1 = 1.f - 2.f * t + BLEP_PHI(ph, t, &q);
            t2 = (float)(ph + q.d) * U2F;
            s1 -= q.c * (1.f - 2.f * t2 + BLEP_PHI(ph + q.d, t2, &q));
            out[i] = q.wsaw * s1;
        }
    }
#ifndef HYB_FAST_VOICE
    else if (q.wave == 0) {             /* (1.18C25: the sine by a phasor turned a sample at each sample, taken exactly
                                         * every 16; within 1e-6 of sy_sin2pi: -120 dB) */
        const float cr = sy_cos2pi((float)q.inc * U2F), sr = sy_sin2pi((float)q.inc * U2F);
        float sn = 0.f, cs = 0.f;
        for (i = 0; i < SY_BLOCK; i++) {
            float y;
            ph += q.inc;
            if ((i & 15) == 0) { const float t = (float)ph * U2F; sn = sy_sin2pi(t); cs = sy_cos2pi(t); }
            else { const float u = cs * cr - sn * sr; sn = sn * cr + cs * sr; cs = u; }
            y = q.ws * sn;
            if (q.wsaw != 0.f) {
                float t = (float)ph * U2F;
                float s1 = 1.f - 2.f * t + BLEP_PH(ph, t, &q);
                if (q.c != 0.f) {
                    float t2 = (float)(ph + q.d) * U2F;
                    s1 -= q.c * (1.f - 2.f * t2 + BLEP_PH(ph + q.d, t2, &q));
                }
                y += q.wsaw * s1;
            }
            out[i] = y;
        }
    }
#endif
    else {
        for (i = 0; i < SY_BLOCK; i++) {
            ph += q.inc;
            out[i] = osc_at(&q, ph);
        }
    }
}
#endif

#if P3_SPAN
/* osc_at_sync's saws (ws = 0) with blep() through the block's constants (span != 0) */
#define BLEP_PK(ph, t, o, k) ((uint32_t)((ph) - (o)->blo) < (o)->bspan ? 0.f : BLEPW(t, k))
static inline __attribute__((always_inline)) float sync_saws_y(const sy_osc_t *o, uint32_t ph, uint32_t phl, const blk_t *k,
                                                               float y)   /* (y: the sine's part, or +0) */
{
    float t = (float)ph * U2F, tl = (float)phl * U2F;
    float s1 = 1.f - 2.f * t + BLEP_PK(ph, t, o, k), sl = 1.f - 2.f * tl + BLEP_PK(phl, tl, o, k);
    float s = (1.f - o->c) * sl;
    if (o->c != 0.f) {
        float t2 = (float)(ph + o->d) * U2F;
        s += o->c * (s1 - (1.f - 2.f * t2 + BLEP_PK(ph + o->d, t2, o, k)));
    }
    y += o->wsaw * s;
    return y;
}
#define sync_saws(o, ph, phl, k) sync_saws_y(o, ph, phl, k, 0.f)
/* (P3_SPAN) osc 2 hard-synced, no FM, its shape the saws alone (ws = 0): vr_osc's sync loop with its quiet runs
 * (osc 2's three phases, the lagged one and the pulse's, clear of their windows, no reset of osc 1 ahead, the
 * lag settled) as lines (P3_RAMP) or per sample; the rest sample by sample as before (the same expressions) */
static SY_HOT __attribute__((noinline)) void osc_sync_span(const sy_osc_t *o, uint32_t *ph2p, uint32_t p1w, uint32_t inc1,
                                                           uint32_t sp0, float *sholdp, uint32_t *slagp, float *out)
{
    const sy_osc_t q2 = *o;
    const uint32_t inc2 = q2.inc, blo = q2.blo, span = q2.bspan, d = q2.d;
    const uint32_t rcp2 = 0xFFFFFFFFu / inc2, rcp1 = 0xFFFFFFFFu / inc1;
    const int pu = q2.c != 0.f;
#if P3_RAMPY
    const float sl = -2.f * (float)inc2 * U2F * (q2.wsaw * (1.f - q2.c));     /* the line's slope a sample */
#endif
    uint32_t ph2 = *ph2p, slag = *slagp;
    float shold = *sholdp;
    int i = 0;
    blk_t k;
    blk_set(&k, q2.dtw, q2.idtw);
    while (i < SY_BLOCK) {                              /* a sample as vr_osc's sync loop; when quiet, the run after it */
        uint32_t q = ph2 += inc2, old = p1w, r1, rl, rd;
        float y0;
        p1w += inc1;
        if (p1w < old) {
#ifdef HYB_FAST_VOICE
            float wr = (float)p1w * (1.f / (float)inc1);
#else
            float wr = (float)p1w / (float)inc1;
#endif
            q = ph2 = sp0 + (uint32_t)(wr * (float)inc2);
            shold = SYNC_HOLD;
            slag = 0;
        } else if (shold > 0.f) {
            float r = shold < 1.f ? shold : 1.f;
            slag += (uint32_t)(r * (float)inc2);
            shold -= r;
        }
        out[i++] = y0 = sync_saws(&q2, q, q - slag, &k);
        r1 = q - blo; rl = q - slag - blo; rd = q + d - blo;
        if (!(shold > 0.f) && r1 < span && rl < span && (!pu || rd < span)) {
            uint32_t m = mulhi32(0xFFFFFFFFu - p1w, rcp1), mq, e;   /* (samples before osc 1's
                                         * wrap, at most) */
            mq = qrun(r1, span, rcp2) - 1u;
            if (mq < m) m = mq;
            mq = qrun(rl, span, rcp2) - 1u;
            if (mq < m) m = mq;
            if (pu) { mq = qrun(rd, span, rcp2) - 1u; if (mq < m) m = mq; }
            e = (uint32_t)(SY_BLOCK - i);
            if (m > e) m = e;
            e = (uint32_t)i + m;
#if P3_RAMPY
            {
                float jf = 1.f;
                for (; i < (int)e; i++, jf += 1.f) out[i] = y0 + sl * jf;
                ph2 += inc2 * m; p1w += inc1 * m;
            }
#else
            for (; i < (int)e; i++) { ph2 += inc2; p1w += inc1; out[i] = sync_saws(&q2, ph2, ph2 - slag, &k); }
#endif
        }
    }
    *ph2p = ph2; *sholdp = shold; *slagp = slag;
}
#endif

#if P3_O2P
/* (P3_O2P) vr_osc's sync / FM loop in two passes: osc 2's phases first (this function: the sync, the FM; written as
 * bit patterns into a free block, w->xt and o3b), then the oscillator at those phases (vr_osc). Each loop gets the
 * registers to itself (the one loop spilled on the host). The same values (bit-identical). */
static inline __attribute__((always_inline)) void put_ph(float *a, int i, uint32_t p) { __builtin_memcpy(a + i, &p, 4); }
static inline __attribute__((always_inline)) uint32_t get_ph(const float *a, int i) { uint32_t p; __builtin_memcpy(&p, a + i, 4); return p; }
#if P3_SYP && P3_SPAN
/* sin(2 pi t) for t in [0, 1]: sin2pi_u's reduction, then an odd polynomial in t (degree 9, fitted on [0, 0.25]: error
 * 3.3e-9; no multiply by 2 pi) */
static inline __attribute__((always_inline)) float sin2pi_q(float t)
{
    float t2;
    t -= t + 0.5f >= 1.f ? 1.f : 0.f;          /* -0.5 .. 0.5 */
    if (t > 0.25f) t = 0.5f - t;
    else if (t < -0.25f) t = -0.5f - t;
    t2 = t * t;
    return t * (6.283185160f + t2 * (-41.34165503f + t2 * (81.60100414f + t2 * (-76.54978357f + t2 * 39.53671465f))));
}
/* (P3_SYP) vr_osc's sync pass after osc_ph2 (P3_SYF): osc 2's saws (ws = 0) or a sine and the saws at the phases pf
 * and the lagged ones pfl; span != 0. SIN: the sine part, PU: the pulse's second saw (c != 0) */
static inline __attribute__((always_inline)) void sync_pass_k(const sy_osc_t *o, const float *restrict pf,
                                                             const float *restrict pfl, float *restrict out, const int SIN,
                                                             const int PU)
{
    const uint32_t blo = o->blo, span = o->bspan, d = o->d;
    const float ws = o->ws, w = o->wsaw, c = o->c, c1 = 1.f - o->c;
    blk_t k;
    int i;
    blk_set(&k, o->dtw, o->idtw);
    for (i = 0; i < SY_BLOCK; i++) {
        const uint32_t ph = get_ph(pf, i), phl = get_ph(pfl, i);
        const float t = (float)ph * U2F, tl = (float)phl * U2F;
        const float sl = 1.f - 2.f * tl + ((uint32_t)(phl - blo) < span ? 0.f : BLEPW(tl, &k));
#if P3_SYQ
        float y = SIN ? ws * sin2pi_q(t) : 0.f;
#else
        float y = SIN ? ws * SIN_PHU(ph, t) : 0.f;
#endif
        float sq = c1 * sl;
        if (PU) {                       /* (s1 only for the pulse: sync_saws_y's value, unused without it) */
            const float s1 = 1.f - 2.f * t + ((uint32_t)(ph - blo) < span ? 0.f : BLEPW(t, &k));
            const float t2 = (float)(ph + d) * U2F;
            sq += c * (s1 - (1.f - 2.f * t2 + ((uint32_t)(ph + d - blo) < span ? 0.f : BLEPW(t2, &k))));
        }
        y += w * sq;
        out[i] = y;
    }
}
static SY_HOT __attribute__((noinline)) void sync_pass(const sy_osc_t *o, const float *restrict pf, const float *restrict pfl,
                                                      float *restrict out)
{
    if (o->ws == 0.f) { if (o->c != 0.f) sync_pass_k(o, pf, pfl, out, 0, 1); else sync_pass_k(o, pf, pfl, out, 0, 0); }
    else if (o->c != 0.f) sync_pass_k(o, pf, pfl, out, 1, 1);
    else sync_pass_k(o, pf, pfl, out, 1, 0);
}
#endif
#if P3_PH2S
/* (P3_PH2S) osc_ph2's sync with the triangle FM (modes 0, 1) on a loop of its own, its state in registers: osc_ph2's
 * expressions, the triangle read from osc 1's phase before its step (tri1's); mode 0 fk = fmk5, fo = 1: fmk5 (tri +
 * 1), mode 1 fk = fmk, fo = 0: fmk (tri + 0): the same products; the FM's floor skipped where the offset lies in
 * [0, 1) (cyc2ph's floor 0 there: c - 0 = c). Bit-identical */
typedef struct { uint32_t ph2, p1w, slag; float shold; } ph2_st_t;
/* (P3_PH2S) the rare part of a sample: osc 1's wrap (osc 2 reset) or the lag still settling; ph2 the phase after its
 * step, returns it (reset or not); st->shold, st->slag updated */
static SY_HOT __attribute__((noinline)) uint32_t ph2_sync(ph2_st_t *st, uint32_t ph2, const uint32_t p1w, const uint32_t old,
                                                         const uint32_t inc1, const uint32_t inc2, const uint32_t sp0)
{
    if (p1w < old) {
#ifdef HYB_FAST_VOICE
        float wr = (float)p1w * (1.f / (float)inc1);
#else
        float wr = (float)p1w / (float)inc1;
#endif
        ph2 = sp0 + (uint32_t)(wr * (float)inc2);
        st->shold = SYNC_HOLD;
        st->slag = 0;
    } else {
        float r = st->shold < 1.f ? st->shold : 1.f;
        st->slag += (uint32_t)(r * (float)inc2);
        st->shold -= r;
    }
    return ph2;
}
static SY_HOT __attribute__((noinline)) void ph2_tri(ph2_st_t *st, const uint32_t inc1, const uint32_t inc2,
                                                    const uint32_t sp0, const float fk, const float fo, float *restrict pf,
                                                    float *restrict pfl)
{
    uint32_t ph2 = st->ph2, p1w = st->p1w, slag = st->slag;
    float shold = st->shold;
    int i;
    for (i = 0; i < SY_BLOCK; i++) {
        const uint32_t old = p1w;
        float u, c;
        ph2 += inc2;
        p1w += inc1;
        if (p1w < old || shold > 0.f) {
            st->shold = shold; st->slag = slag;
            ph2 = ph2_sync(st, ph2, p1w, old, inc1, inc2, sp0);
            shold = st->shold; slag = st->slag;
        }
        u = (float)old * U2F;
        c = fk * ((u < 0.5f ? 1.f - 4.f * u : 4.f * u - 3.f) + fo);
        ph2 += c >= 0.f && c < 1.f ? (uint32_t)(c * 4294967040.f) : cyc2ph(c);
        put_ph(pf, i, ph2);
        put_ph(pfl, i, ph2 - slag);
    }
    st->ph2 = ph2; st->p1w = p1w; st->slag = slag; st->shold = shold;
}
#endif
static SY_HOT __attribute__((noinline)) void osc_ph2(sy_voice_t *v, const sy_vblk_t *b, const float *in, uint32_t ph1,
                                                    uint32_t *ph2p, float *sholdp, uint32_t *slagp, float *restrict pf,
                                                    float *restrict pfl)
{
    const float fmk = (float)b->o2.inc * U2F * b->fm_i, fmk5 = fmk * 0.5f, fmkn = fmk * NFM_K * b->nf_gain;
    const uint32_t ph1_0 = ph1 - b->o1.inc * (uint32_t)SY_BLOCK;
    const uint32_t inc2 = b->o2.inc, inc1 = b->o1.inc, sp0 = b->sync_p0;
    const int sy = b->sync, fmm = b->fm_mode, fm = b->fm_i != 0.f;
    float shold = *sholdp;
    uint32_t slag = *slagp, p1w = ph1_0, ph2 = *ph2p;
    int i;
#ifdef HYB_FAST_VOICE
    const float rinc1 = 1.f / (float)inc1;
#endif
#if P3_PH2S
    if (sy && fm && fmm <= 1) {         /* (P3_PH2S) the sync with the triangle FM: ph2_tri */
        ph2_st_t st;
        st.ph2 = ph2; st.p1w = p1w; st.slag = slag; st.shold = shold;
        ph2_tri(&st, inc1, inc2, sp0, fmm ? fmk : fmk5, fmm ? 0.f : 1.f, pf, pfl);
        *ph2p = st.ph2; *sholdp = st.shold; *slagp = st.slag;
        return;
    }
#endif
    for (i = 0; i < SY_BLOCK; i++) {
        uint32_t p = ph2 += inc2;
        if (sy) {               /* osc 1's wrap this sample (the wrap's fraction of a step, as a float) resets osc 2 */
            uint32_t old = p1w;
            p1w += inc1;
            if (p1w < old) {
#ifdef HYB_FAST_VOICE
                float wr = (float)p1w * rinc1;
#else
                float wr = (float)p1w / (float)inc1;
#endif
                p = ph2 = sp0 + (uint32_t)(wr * (float)inc2);
                shold = SYNC_HOLD;
                slag = 0;
            } else if (shold > 0.f) {
                float r = shold < 1.f ? shold : 1.f;
                slag += (uint32_t)(r * (float)inc2);
                shold -= r;
            }
        }
        if (fm) {
            float m;
            switch (fmm) {
            case 0:             /* linear FM by a unipolar triangle read from osc 1's phase (its
                                 * shape does not matter: read by demodulating a sine carrier, sine and saw
                                 * modulators alike), the top at osc 1's wrap: osc 2 rises up to fm_i */
                ph2 += cyc2ph(fmk5 * (1.f + tri1(ph1_0, b->o1.inc, i)));
                p = ph2;
                break;
            case 1:             /* the same triangle, bipolar */
                ph2 += cyc2ph(fmk * tri1(ph1_0, b->o1.inc, i));
                p = ph2;
                break;
            case 2:             /* linear FM by osc 1's output, bipolar; inverted (osc 2 falls while
                                 * osc 1 is positive: the modulator phase read with reset phases, 1:1, 2:1) */
                {               /* the modulator is osc 1's wave (shape 0-63, a unit peak) or
                                 * a plain falling saw (64-127, the peak 1), whatever the shape's mix:
                                 * one modulation for each half (read by demodulation) */
                    uint32_t q1 = ph1 - b->o1.inc * (uint32_t)(SY_BLOCK - i);   /* (one sample late) */
                    m = b->fm_saw ? 1.f - 2.f * (float)q1 * U2F : wave_at(&b->o1, q1);
                }
                ph2 -= cyc2ph(fmk * m);
                p = ph2;
                break;
            case 3:             /* linear FM of osc 2 by the noise, coloured by the noise
                                 * oscillator's low pass (B 39 below 64; synth.c: its level per colour): read as a band of noise around osc 2's pitch, -21 dB
                                 * re the carrier in the third octave next to it at amount 16, the carrier
                                 * itself falling from 64 on */
                {
                    float n = sy_randf(&v->rng);
                    if (b->n_mode == 1) {
                        float yv = b->n_lpa * (n + v->nf_x) - b->n_lpb * v->nf_lp;
                        v->nf_x = n; v->nf_lp = yv; n = yv;
                    }
                    ph2 += cyc2ph(fmkn * n);
                    p = ph2;
                }
                break;
            case 4:             /* the audio input */
                p += rad2ph(b->fm_i * (in ? in[i] : 0.f));
                break;
            default:
                break;
            }
        }
        put_ph(pf, i, p);
        if (sy) put_ph(pfl, i, p - slag);
    }
    *ph2p = ph2; *sholdp = shold; *slagp = slag;
}
#endif

/* CLEANER: the 6-voice state on the plain oscillators (osc 1 and 2 off the own clock: sync, FM, the input's FM; synth.c
 * CO_V6, PROVENANCE 9d): each oscillator through a 3-tap FIR, the reference's line ratio 6 / 1 voices as its own
 * clock's oscillators show it (CO_P6_1: osc 1's, CO_P6_2: osc 2's), the VC6 gain apart. Fitted, not exact: the ratio
 * moves with the clock's rate by the key (+-1 dB at 6-12 kHz, more towards 20 kHz); the fit within 0.35 dB below
 * 12 kHz on the keys' mean. Off the state, the last two samples only (no step when it starts) */
static SY_HOT void pl6(float *restrict x, float *st, const float *h, int on)
{
    float x1 = st[0], x2 = st[1];
    int i;
    if (on)
        for (i = 0; i < SY_BLOCK; i++) { const float u = x[i]; x[i] = h[0] * u + h[1] * x1 + h[2] * x2; x2 = x1; x1 = u; }
    else { x1 = x[SY_BLOCK - 1]; x2 = x[SY_BLOCK - 2]; }
    st[0] = x1; st[1] = x2;
}

#if P3_PL6
/* (P3_PL6) pl6 on osc 1 and 2 in one pass (the same expressions: bit-identical) */
static SY_HOT void pl6x2(float *restrict x, float *restrict y, float *st, const float *h, const float *g)
{
    const float h0 = h[0], h1 = h[1], h2 = h[2], g0 = g[0], g1 = g[1], g2 = g[2];
    float x1 = st[0], x2 = st[1], y1 = st[2], y2 = st[3];
    int i;
    for (i = 0; i < SY_BLOCK; i++) {
        const float u = x[i], w = y[i];
        x[i] = h0 * u + h1 * x1 + h2 * x2; x2 = x1; x1 = u;
        y[i] = g0 * w + g1 * y1 + g2 * y2; y2 = y1; y1 = w;
    }
    st[0] = x1; st[1] = x2; st[2] = y1; st[3] = y2;
}
#endif
/* (CLEANER, RING_SUB) the sub's part of the ring modulator: (osc 1 + sub) x osc 2, the sub's product folded into the
 * sub's block with its balance gain, xs (sub_gb - ring_s osc 2), before the mix (the ring enters inverted against the
 * mix; the sub under osc 1's side of the balance while the ring is on: synth.c). In the flash
 * (only patches with both the ring and the sub on reach it); in the tick path osc 2 is read per sample by the plain
 * oscillator into o2 (scratch; the tick clock's osc 2 has no per-sample values) */
static __attribute__((noinline)) void sub_ring(const sy_vblk_t *b, sy_vwork_t *w, const float *o2)
{
    const float rs = b->ring_s, gb = b->sub_gb;
    float *xs = w->xs;
    int i;
#if P3_O2S
    if (b->tk) {                        /* (P3_O2S: osc 2's samples from the tick path's own oscillator when it left them
                                         * in w->o2s; no producer yet: see sy_vwork_t) */
        if (w->o2s) o2 = w->o2s;
        else { SY_FAR(osc_run)(&b->o2, w->ph2_0, w->o2b, 1); o2 = w->o2b; }
    }
#else
    if (b->tk) { SY_FAR(osc_run)(&b->o2, w->ph2_0, w->o2b, 1); o2 = w->o2b; }
#endif
    for (i = 0; i < SY_BLOCK; i++) xs[i] *= gb - rs * o2[i];
}

#if P3_MIX2
/* (P3_MIX2) the plain path's mix with osc 3 or the ring, its sum (the low pass after it in vr_osc): k = gb, rs, g1, g2,
 * g3, rg, r1, r2 (gb 1, rs 0 without RING_SUB: xs[i] x 1, exact); f: 1 osc 3, 2 the ring. A function of its own, one
 * loop per case: the target's compiler kept vr_osc's low-pass state and the constants on the stack in one loop */
static inline __attribute__((always_inline)) void mix_sum_k(float *restrict xo, const float *xs, const float *o1b,
                                                           const float *o2b, const float *o3b, const float *k, const int O3,
                                                           const int RG)
{
    const float gb = k[0], rs = k[1], g1 = k[2], g2 = k[3], g3 = k[4], rg = k[5], r1 = k[6], r2 = k[7];
    int i;
    for (i = 0; i < SY_BLOCK; i++) {
        float x = xs[i] * (gb - rs * o2b[i]);
        x += g1 * o1b[i] + g2 * o2b[i];
        if (O3) x += g3 * o3b[i];
#if P3_RGK
        if (RG) x -= (o1b[i] * rg) * o2b[i];   /* (rg here: rg r1 r2, vr_osc) */
        (void)r1; (void)r2;
#else
        if (RG) x -= rg * (o1b[i] * r1) * (o2b[i] * r2);
#endif
        xo[i] = x;
    }
}
static SY_HOT __attribute__((noinline)) void mix_sum(float *restrict xo, const float *xs, const float *o1b, const float *o2b,
                                                   const float *o3b, const float *k, int f)
{
    if (f == 3) mix_sum_k(xo, xs, o1b, o2b, o3b, k, 1, 1);
    else if (f == 2) mix_sum_k(xo, xs, o1b, o2b, o3b, k, 0, 1);
    else mix_sum_k(xo, xs, o1b, o2b, o3b, k, 1, 0);
}
#endif
#if P3_LPN
/* (P3_LPN) the mix's low pass on xo in place and the noise added (vr_osc's two passes in one: lp, then + g n with
 * the noise's colour as vr_osc's noise stage; the same expressions); the states back into v */
static inline __attribute__((always_inline)) void lpn_k(sy_voice_t *v, const sy_vblk_t *b, float *restrict xo, float lp,
                                                       float xp, const int NM)
{
    const float oa = b->olp_a, ob = b->olp_b, ng = b->noise_g * b->n_gain, lpa = b->n_lpa, lpb = b->n_lpb, hpa = b->n_hpa;
    float nl = v->n_lp, nh = v->n_hp, nx = v->n_x;
    uint32_t rs = v->rng;
    int i;
    for (i = 0; i < SY_BLOCK; i++) {
        const float x = xo[i], y = oa * (x + xp) - ob * lp;
        float n = sy_randf(&rs);
        xp = x;
        lp = y;
        if (NM == 1) { const float yv = lpa * (n + nx) - lpb * nl; nx = n; nl = yv; n = yv; }
        else if (NM == 2) { nh += hpa * (n - nh); n -= nh; }
        xo[i] = lp + ng * n;
    }
    v->lp_osc = lp; v->lp_x = xp;
    v->rng = rs; v->n_lp = nl; v->n_x = nx; v->n_hp = nh;
}
static SY_HOT __attribute__((noinline)) void lpn(sy_voice_t *v, const sy_vblk_t *b, float *restrict xo, float lp, float xp)
{
    if (b->n_mode == 1) lpn_k(v, b, xo, lp, xp, 1);
    else if (b->n_mode == 2) lpn_k(v, b, xo, lp, xp, 2);
    else lpn_k(v, b, xo, lp, xp, 0);
}
#endif
/* the voice in three stages, each its own function: the compiler gives each loop its registers (one function
 * of all three spilled in one loop whatever changed in another) */
static SY_HOT __attribute__((noinline)) void vr_osc(sy_voice_t *v, const sy_vblk_t *b, const float *in, sy_vwork_t *w, int nzpost,
                                             int split)
{
    float *restrict xo = w->xo, *restrict xo2 = w->xo2, *restrict xn = w->xn, xt0 = 0.f;
    int i;
    uint32_t ph1 = v->ph1, ph2 = v->ph2, ph3 = v->ph3, phs = v->phs;
    int need_o1 = b->g1 != 0.f || b->ring_g != 0.f || b->fm_i != 0.f || b->sync;

    /* -- the oscillators and the mix in front of the filters */
    if (b->use_input) {
        for (i = 0; i < SY_BLOCK; i++) xo[i] = in ? in[i] * b->in_g : 0.f;
    } else {
        float lp = v->lp_osc, slp = v->lp_sub, xp = v->lp_x;
        float *o1b = w->o1b, *o2b = w->o2b, *restrict o3b = w->o3b, *xs = w->xs;   /* (o1b, o2b, xs: or the zero block) */
        int on2 = b->g2 != 0.f || b->ring_g != 0.f;
        int lpn_done = 0;               /* (P3_LPN: the noise added with the mix's low pass) */
        (void)slp;
        /* one pass per oscillator (the decisions outside the loops), then the mix */
        float *xt = w->xt;              /* (not restrict: xo itself when tk_only) */
        /* (CLEANER, 1.18C25) osc 1 and 2 on the ticks alone (no sub, no osc 3, the mix's low pass at rest: it adds
         * +0): written into xo as they are, no mix pass */
        const int tk_only = b->tk && !split && b->sub_g == 0.f && !b->o3_on && lp == 0.f && xp == 0.f;
        if (tk_only) xt = xo;
        if (b->tk) {                    /* osc 1 and 2 on the tick clock */
            w->ph2_0 = ph2;             /* (osc 2's phase at the block's start: sub_ring) */
#if P3_O2S
            w->o2s = 0;                 /* (cln_osc may leave osc 2's samples there for sub_ring) */
#endif
            cln_osc(v, b, xt, &xt0, w->o1b);   /* (xt0: the start transient's sign, the waves' naive mix at the first
                                         * tick; o1b, o2b its scratch) */
            ph1 = v->ph1; ph2 = v->ph2;
            o1b = o2b = (float *)sy_zero_blk;   /* (not read by the tick mix: nothing to zero) */
        } else {
        if (need_o1) osc_run(&b->o1, ph1, o1b, 0);
        else o1b = (float *)sy_zero_blk;     /* (a source off: the mix reads +0 from a shared block) */
        ph1 += b->o1.inc * (uint32_t)SY_BLOCK;
        }
        if (b->sub_g != 0.f) {          /* the sub, one octave down: a full-band square (shape 0) or triangle
                                         * (shape 1-127), as read on the reference: lines 1/n (1/n^2) up to ~15 kHz */
            float sg = b->sub_g, dt = b->sub_dt;
            const uint32_t sinc = b->sub_inc;   /* (a local: xs[] is not restrict, so b->sub_inc would be loaded per sample) */
            if (b->sub_tri == 0.f) {
                float wf = sg * 0.785398f;          /* unit fundamental */
                uint32_t blo, bspan, blo2;
                float idt = 1.f / dt;
                sy_blep_window(dt, &blo, &bspan);   /* (blep() skipped where it is surely 0: u2 is the phase half a
                                         * cycle on, within the window's margin) */
                blo2 = blo - 0x80000000u;   /* (the window's start for the phase half a cycle on: one add less) */
#if P3_EV || P3_EVTK
                if ((P3_EV || b->tk) && bspan != 0u && sinc != 0u && sy_absf((float)sinc * U2F * idt - 0.5f) < 1e-5f) {    /* (P3_EV: lines and
                                         * jumps; dt is two samples: 8 points on per sample) */
                    blk_t k;
                    blk_set(&k, dt, idt);
                    osc_saw2(xs, phs - SUB_SQ_C, sinc, 0x80000000u, wf, -wf, &k, 1, 8);
                    phs += sinc * (uint32_t)SY_BLOCK;
                } else
#endif
#if P3_SPAN
                if (bspan != 0u && sinc != 0u) {    /* (P3_SPAN: the quiet runs between the edges' windows) */
                    const uint32_t rcp = 0xFFFFFFFFu / sinc;
                    blk_t k;
                    blk_set(&k, dt, idt);
                    i = 0;
                    while (i < SY_BLOCK) {      /* a sample as before; when quiet, the run after it */
                        uint32_t p, r1, r2;
                        float u, u2, b1, b2, y0;
                        phs += sinc;
                        p = phs - SUB_SQ_C;
                        r1 = p - blo; r2 = p - blo2;
                        u = (float)p * U2F;
                        u2 = u + 0.5f;
                        if (u2 >= 1.f) u2 -= 1.f;
                        b1 = r1 < bspan ? 0.f : BLEPW(u, &k);
                        b2 = r2 < bspan ? 0.f : BLEPW(u2, &k);
                        xs[i++] = y0 = wf * ((b1 - (u + u)) - (b2 - (u2 + u2)));
                        if (r1 < bspan && r2 < bspan) {
                            uint32_t m = qrun(r1, bspan, rcp) - 1u, m2 = qrun(r2, bspan, rcp) - 1u, e = (uint32_t)(SY_BLOCK - i);
                            if (m2 < m) m = m2;
                            if (m > e) m = e;
                            e = (uint32_t)i + m;
#if P3_RAMPQ
                            for (; i < (int)e; i++) xs[i] = y0;     /* (the square's level held) */
                            phs += sinc * m;
#else
#if P3_SUBQ
                            {                   /* (P3_SUBQ) wf ((0 - 2u) - (0 - 2u2)) = (2 wf) (u2 - u) exactly (the
                                         * negations and the factor 2 exact, a zero's sign as before); u2's wrap is
                                         * the same over a quiet run (it lies between the edges, u = 0 and 0.5) */
                                const float wf2 = wf + wf;
                                p = phs - SUB_SQ_C;
                                if (u >= 0.5f) for (; i < (int)e; i++) {    /* (u2 = u + 0.5 - 1) */
                                    p += sinc;
                                    u = (float)p * U2F;
                                    xs[i] = wf2 * ((u + 0.5f) - 1.f - u);
                                } else for (; i < (int)e; i++) {
                                    p += sinc;
                                    u = (float)p * U2F;
                                    xs[i] = wf2 * ((u + 0.5f) - u);
                                }
                                phs = p + SUB_SQ_C;
                            }
#else
                            for (; i < (int)e; i++) {
                                phs += sinc;
                                p = phs - SUB_SQ_C;
                                u = (float)p * U2F;
                                u2 = u + 0.5f;
                                if (u2 >= 1.f) u2 -= 1.f;
                                b1 = 0.f; b2 = 0.f;
                                xs[i] = wf * ((b1 - (u + u)) - (b2 - (u2 + u2)));
                            }
#endif
#endif
                        }
                    }
                } else
#endif
                for (i = 0; i < SY_BLOCK; i++) {
                    float u, u2, b1, b2;
                    uint32_t p;
                    phs += sinc;
                    p = phs - SUB_SQ_C;
                    u = (float)p * U2F;                         /* (the square a quarter behind the triangle) */
                    u2 = u + 0.5f;
                    if (u2 >= 1.f) u2 -= 1.f;  /* (u2 in [0.5, 1.5): as u2 - (float)(int)u2, without the conversions) */
                    b1 = (uint32_t)(p - blo) < bspan ? 0.f : blep(u, dt, idt);
                    b2 = (uint32_t)(p - blo2) < bspan ? 0.f : blep(u2, dt, idt);
                    xs[i] = wf * ((b1 - (u + u)) - (b2 - (u2 + u2)));   /* (2u as u + u: the same value) */
                }
            } else {                    /* a naive triangle, its corners rounded by a 2-point polyBLAMP */
                float wt = sg * SUB_TRI_G * 1.2337006f, ds = sinc * U2F, ids = 1.f / ds;
#if P3_SUBT
                if (sinc != 0u && sinc < 0x08000000u) {   /* (P3_SUBT) the samples clear of the corners as lines */
                    const uint32_t rcp = 0xFFFFFFFFu / sinc, s2 = sinc + sinc;
                    const float sl = 4.f * ds * wt;
                    i = 0;
                    while (i < SY_BLOCK) {      /* a sample as before; clear of both corners, the run after it */
                        float u, g, x, y0;
                        uint32_t fa, fb, f;
                        phs += sinc;
                        u = (float)phs * U2F;
                        g = u < 0.25f ? 4.f * u : u < 0.75f ? 2.f - 4.f * u : 4.f * u - 4.f;
                        x = (u - 0.25f) * ids;
                        if (x > -1.f && x < 1.f) g -= 8.f * ds * (x < 0.f ? (1.f + x) * (1.f + x) * (1.f + x) : (1.f - x) * (1.f - x) * (1.f - x)) * (1.f / 6.f);
                        x = (u - 0.75f) * ids;
                        if (x > -1.f && x < 1.f) g += 8.f * ds * (x < 0.f ? (1.f + x) * (1.f + x) * (1.f + x) : (1.f - x) * (1.f - x) * (1.f - x)) * (1.f / 6.f);
                        xs[i++] = y0 = wt * g;
                        fa = 0x40000000u - phs; fb = 0xC0000000u - phs;   /* (the corners ahead, as phase distances) */
                        f = fa < fb ? fa : fb;
                        if (f > s2 && (uint32_t)(phs - 0x40000000u) >= sinc && (uint32_t)(phs - 0xC0000000u) >= sinc) {
                            uint32_t m = mulhi32(f - s2, rcp), e = (uint32_t)(SY_BLOCK - i);   /* (the samples
                                         * ahead that stay two steps short of the next corner) */
                            const float sg2 = fa < fb ? sl : -sl;   /* (rising towards 0.25, falling towards 0.75) */
                            float jf = 1.f;
                            if (m > e) m = e;
                            e = (uint32_t)i + m;
                            for (; i < (int)e; i++, jf += 1.f) xs[i] = y0 + sg2 * jf;
                            phs += sinc * m;
                        }
                    }
                } else
#endif
                for (i = 0; i < SY_BLOCK; i++) {
                    float u, g, x;
                    phs += sinc;
                    u = (float)phs * U2F;
                    g = u < 0.25f ? 4.f * u : u < 0.75f ? 2.f - 4.f * u : 4.f * u - 4.f;
                    x = (u - 0.25f) * ids;              /* the corner at 0.25: the slope changes by -8 */
                    if (x > -1.f && x < 1.f) g -= 8.f * ds * (x < 0.f ? (1.f + x) * (1.f + x) * (1.f + x) : (1.f - x) * (1.f - x) * (1.f - x)) * (1.f / 6.f);
                    x = (u - 0.75f) * ids;              /* at 0.75: by +8 */
                    if (x > -1.f && x < 1.f) g += 8.f * ds * (x < 0.f ? (1.f + x) * (1.f + x) * (1.f + x) : (1.f - x) * (1.f - x) * (1.f - x)) * (1.f / 6.f);
                    xs[i] = wt * g;
                }
            }
        } else xs = (float *)sy_zero_blk;
        if (b->tk) ;
        else if (on2 && !b->sync && b->fm_i == 0.f) {
            osc_run(&b->o2, ph2, o2b, 0);
            ph2 += b->o2.inc * (uint32_t)SY_BLOCK;
        } else if (on2) {
            /* (the FM's block constants, as the expressions' first factors: the same products, once) */
            const float fmk = (float)b->o2.inc * U2F * b->fm_i, fmk5 = fmk * 0.5f, fmkn = fmk * NFM_K * b->nf_gain;
            const uint32_t ph1_0 = ph1 - b->o1.inc * (uint32_t)SY_BLOCK;
            /* (the block's constants in locals and osc 2 in a copy: the stores to o2b[] could alias b and v, so
             * through the pointers every field would be loaded again per sample) */
            const sy_osc_t q2 = b->o2;
            const uint32_t inc2 = q2.inc, inc1 = b->o1.inc, sp0 = b->sync_p0;
            const int sy = b->sync, fmm = b->fm_mode, fm = b->fm_i != 0.f;
            float shold = v->sync_hold;
            uint32_t slag = v->sync_lag, p1w = ph1_0;   /* (p1w: osc 1's phase, for its wrap) */
#ifdef HYB_FAST_VOICE
            const float rinc1 = 1.f / (float)inc1;
#endif
#if P3_SPAN
            if (sy && !fm && q2.ws == 0.f && q2.wsaw != 0.f && q2.bspan != 0u && inc2 != 0u && inc1 != 0u)
                osc_sync_span(&q2, &ph2, p1w, inc1, sp0, &shold, &slag, o2b);
            else
#endif
#if P3_O2P
            {
                float *pf = w->xt, *pfl = o3b;  /* (free here: xt is the tick path's, o3b osc 3's, rendered below) */
                osc_ph2(v, b, in, ph1, &ph2, &shold, &slag, pf, pfl);
#if P3_SYF && P3_SPAN
                if (sy && q2.bspan != 0u && q2.wsaw != 0.f && (q2.ws == 0.f || q2.wave == 0)) {   /* (P3_SYF) */
#if P3_SYP
                    sync_pass(&q2, pf, pfl, o2b);
#else
                    blk_t k;
                    blk_set(&k, q2.dtw, q2.idtw);
                    if (q2.ws == 0.f) for (i = 0; i < SY_BLOCK; i++) o2b[i] = sync_saws(&q2, get_ph(pf, i), get_ph(pfl, i), &k);
                    else for (i = 0; i < SY_BLOCK; i++) {
                        const uint32_t p = get_ph(pf, i);
                        const float t = (float)p * U2F;
                        o2b[i] = sync_saws_y(&q2, p, get_ph(pfl, i), &k, q2.ws * SIN_PHU(p, t));
                    }
#endif
                } else
#endif
                if (sy) for (i = 0; i < SY_BLOCK; i++) o2b[i] = osc_at_sync(&q2, get_ph(pf, i), get_ph(pfl, i));
                else if (q2.ws == 0.f && q2.wsaw != 0.f) {     /* (the saws alone: osc_at's terms, its tests once) */
                    const float w2 = q2.wsaw, c2 = q2.c;
                    if (c2 != 0.f) for (i = 0; i < SY_BLOCK; i++) {
                        const uint32_t p = get_ph(pf, i);
                        const float t = (float)p * U2F, t2 = (float)(p + q2.d) * U2F;
                        float y = 0.f, s1 = 1.f - 2.f * t + BLEP_PH(p, t, &q2);
                        s1 -= c2 * (1.f - 2.f * t2 + BLEP_PH(p + q2.d, t2, &q2));
                        y += w2 * s1;
                        o2b[i] = y;
                    } else for (i = 0; i < SY_BLOCK; i++) {
                        const uint32_t p = get_ph(pf, i);
                        const float t = (float)p * U2F;
                        float y = 0.f;
                        y += w2 * (1.f - 2.f * t + BLEP_PH(p, t, &q2));
                        o2b[i] = y;
                    }
                } else for (i = 0; i < SY_BLOCK; i++) o2b[i] = osc_at(&q2, get_ph(pf, i));
                (void)fmk5; (void)fmkn; (void)fmm;
#ifdef HYB_FAST_VOICE
                (void)rinc1;
#endif
            }
#else
            for (i = 0; i < SY_BLOCK; i++) {
                uint32_t p = ph2 += inc2;
                if (sy) {               /* osc 1's wrap this sample (the wrap's fraction of a step, as a float) resets osc 2 */
                    uint32_t old = p1w;
                    p1w += inc1;
                    if (p1w < old) {
#ifdef HYB_FAST_VOICE
                        float wr = (float)p1w * rinc1;
#else
                        float wr = (float)p1w / (float)inc1;
#endif
                        p = ph2 = sp0 + (uint32_t)(wr * (float)inc2);
                        shold = SYNC_HOLD;
                        slag = 0;
                    } else if (shold > 0.f) {
                        float r = shold < 1.f ? shold : 1.f;
                        slag += (uint32_t)(r * (float)inc2);
                        shold -= r;
                    }
                }
                if (fm) {
                    float m;
                    switch (fmm) {
                    case 0:             /* linear FM by a unipolar triangle read from osc 1's phase (its
                                         * shape does not matter: read by demodulating a sine carrier, sine and saw
                                         * modulators alike), the top at osc 1's wrap: osc 2 rises up to fm_i */
                        ph2 += cyc2ph(fmk5 * (1.f + tri1(ph1_0, b->o1.inc, i)));
                        p = ph2;
                        break;
                    case 1:             /* the same triangle, bipolar */
                        ph2 += cyc2ph(fmk * tri1(ph1_0, b->o1.inc, i));
                        p = ph2;
                        break;
                    case 2:             /* linear FM by osc 1's output, bipolar; inverted (osc 2 falls while
                                         * osc 1 is positive: the modulator phase read with reset phases, 1:1, 2:1) */
                        {               /* the modulator is osc 1's wave (shape 0-63, a unit peak) or
                                         * a plain falling saw (64-127, the peak 1), whatever the shape's mix:
                                         * one modulation for each half (read by demodulation) */
                            uint32_t q1 = ph1 - b->o1.inc * (uint32_t)(SY_BLOCK - i);   /* (one sample late) */
                            m = b->fm_saw ? 1.f - 2.f * (float)q1 * U2F : wave_at(&b->o1, q1);
                        }
                        ph2 -= cyc2ph(fmk * m);
                        p = ph2;
                        break;
                    case 3:             /* linear FM of osc 2 by the noise, coloured by the noise
                                         * oscillator's low pass (B 39 below 64; synth.c: its level per colour): read as a band of noise around osc 2's pitch, -21 dB
                                         * re the carrier in the third octave next to it at amount 16, the carrier
                                         * itself falling from 64 on */
                        {
                            float n = sy_randf(&v->rng);
                            if (b->n_mode == 1) {
                                float yv = b->n_lpa * (n + v->nf_x) - b->n_lpb * v->nf_lp;
                                v->nf_x = n; v->nf_lp = yv; n = yv;
                            }
                            ph2 += cyc2ph(fmkn * n);
                            p = ph2;
                        }
                        break;
                    case 4:             /* the audio input */
                        p += rad2ph(b->fm_i * (in ? in[i] : 0.f));
                        break;
                    default:
                        break;
                    }
                }
                o2b[i] = sy ? osc_at_sync(&q2, p, p - slag) : osc_at(&q2, p);
            }
#endif
            if (sy) { v->sync_hold = shold; v->sync_lag = slag; }
        } else o2b = (float *)sy_zero_blk;
        if (b->o3_on) {
            osc_run(&b->o3, ph3, o3b, b->tk);    /* (P3_EVTK: osc 3 on the tick path) */
            ph3 += b->o3.inc * (uint32_t)SY_BLOCK;
        }
        if (!b->tk) {                   /* (the plain oscillators: the 6-voice state's FIR, or its history) */
            static const float H1[3] = {CO_P6_1}, H2[3] = {CO_P6_2};
#if P3_PL6
            if (b->pl_v6 && o1b != sy_zero_blk && o2b != sy_zero_blk) pl6x2(o1b, o2b, v->p6, H1, H2);
            else
#endif
            {
            if (o1b != sy_zero_blk) pl6(o1b, v->p6, H1, b->pl_v6);
            if (o2b != sy_zero_blk) pl6(o2b, v->p6 + 2, H2, b->pl_v6);
            }
        }
#if P3_MIX2
        const int rsub = !b->tk && !split && b->ring_s != 0.f;   /* (the plain path's mix takes it, osc 3 or not) */
#elif P3_MIX
        const int rsub = !b->tk && !split && !b->o3_on && b->ring_s != 0.f;   /* (the plain path's mix takes it) */
#else
        const int rsub = 0;
#endif
        if (b->ring_s != 0.f && !split && !rsub) {   /* (RING_SUB: the sub in the ring's product; xs is the voice's own
                                         * block here, the sub being on) */
            SY_FAR(sub_ring)(b, w, o2b);
        }
        {
            float g1 = b->g1, g2 = b->g2, g3 = b->g3, rg = b->ring_g, r1 = b->o1.ring, r2 = b->o2.ring;
            float oa = b->olp_a, ob = b->olp_b;
            int o3 = b->o3_on;
            if (split) {                /* (read: each oscillator on its own side at its balance gain; the ring
                                         * modulator heard on neither; osc 3 with osc 2) */
                float lp2 = v->lp_osc2, xp2 = v->lp_x2;
                for (i = 0; i < SY_BLOCK; i++) {
                    float x = xs[i] + g1 * o1b[i], x2 = g2 * o2b[i], y;
                    if (o3) x2 += g3 * o3b[i];
                    y = oa * (x + xp) - ob * lp; xp = x; lp = y; xo[i] = lp;
                    y = oa * (x2 + xp2) - ob * lp2; xp2 = x2; lp2 = y; xo2[i] = lp2;
                }
                v->lp_osc2 = lp2; v->lp_x2 = xp2;
            } else
            if (tk_only) ;
            else
            if (b->tk) for (i = 0; i < SY_BLOCK; i++) {    /* the sub and osc 3 as before, osc 1 and 2 from the ticks */
                float x = xs[i], y;
                if (o3) x += g3 * o3b[i];
                y = oa * (x + xp) - ob * lp;
                xp = x;
                lp = y;
                xo[i] = lp + xt[i];
            } else
            if (!o3 && rg == 0.f)       /* (the common mix: no osc 3, no ring; the same sums) */
            for (i = 0; i < SY_BLOCK; i++) {
                float x = xs[i], y;
                x += g1 * o1b[i] + g2 * o2b[i];
                y = oa * (x + xp) - ob * lp;
                xp = x;
                lp = y;
                xo[i] = lp;
            }
            else
#if P3_MIX2
            {                           /* (P3_MIX2: osc 3 or the ring: the sum into xo, RING_SUB's factor on the sub
                                         * (sub_ring's: xs[i] x (gb - rs o2b[i]); without it xs[i] x 1, exact), then
                                         * the low pass in place; the same expressions in the same order) */
#if P3_RGK
                const float k[8] = {rsub ? b->sub_gb : 1.f, rsub ? b->ring_s : 0.f, g1, g2, g3, rg * r1 * r2, r1, r2};
#else
                const float k[8] = {rsub ? b->sub_gb : 1.f, rsub ? b->ring_s : 0.f, g1, g2, g3, rg, r1, r2};
#endif
                mix_sum(xo, xs, o1b, o2b, o3b, k, (o3 ? 1 : 0) | (rg != 0.f ? 2 : 0));
#if P3_LPN
                if (b->noise_g != 0.f && !nzpost && b->f1x == 0.f && !b->dip
                    && !(v->click < (float)(CLKTAB_N - 1) && b->clk_g != 0.f)) {   /* (the noise straight into the mix) */
                    lpn(v, b, xo, lp, xp);
                    lp = v->lp_osc; xp = v->lp_x;
                    lpn_done = 1;
                } else
#endif
                for (i = 0; i < SY_BLOCK; i++) {
                    const float x = xo[i], y = oa * (x + xp) - ob * lp;
                    xp = x;
                    lp = y;
                    xo[i] = lp;
                }
            }
#else
#if P3_MIX
            if (!o3) {                  /* (P3_MIX: the ring without osc 3, RING_SUB's factor on the sub here: sub_ring's
                                         * and the mix's expressions, bit-identical) */
                const float rs = rsub ? b->ring_s : 0.f, gb = rsub ? b->sub_gb : 1.f;   /* (without RING_SUB xs[i] x 1:
                                         * exact) */
                for (i = 0; i < SY_BLOCK; i++) {
                    float x = xs[i] * (gb - rs * o2b[i]), y;
                    x += g1 * o1b[i] + g2 * o2b[i];
                    x -= rg * (o1b[i] * r1) * (o2b[i] * r2);
                    y = oa * (x + xp) - ob * lp;
                    xp = x;
                    lp = y;
                    xo[i] = lp;
                }
            } else
#endif
            for (i = 0; i < SY_BLOCK; i++) {
                float x = xs[i], y;
                x += g1 * o1b[i] + g2 * o2b[i];
                if (o3) x += g3 * o3b[i];
                if (rg != 0.f) x -= rg * (o1b[i] * r1) * (o2b[i] * r2);     /* the ring enters inverted against the mix */
                y = oa * (x + xp) - ob * lp;    /* the top roll-off of the partly band-limited oscillators (bilinear) */
                xp = x;
                lp = y;
                xo[i] = lp;
            }
#endif
        }
        v->lp_osc = lp;
        v->lp_x = xp;
        if (b->dip) {                   /* the high keys' dip (synth.c) */
            float z1 = v->dip1, z2 = v->dip2, b0 = b->dp[0], b1 = b->dp[1], b2 = b->dp[2], a1 = b->dp[3], a2 = b->dp[4];
            for (i = 0; i < SY_BLOCK; i++) {
                float x = xo[i], y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                xo[i] = y;
            }
            v->dip1 = z1; v->dip2 = z2;
        } else v->dip1 = v->dip2 = 0.f;
        if (v->click < (float)(CLKTAB_N - 1) && b->clk_g != 0.f) {
            /* the start transient, as read (sy_hyb_click.h): a pulse of the punch's size, its time by the pitch */
            float t = v->click, r = b->clk_rate, g = b->clk_g;
            if (t == 0.f) {             /* the pulse takes the sign of the oscillators' sum as the note starts
                                         * (read with free-running sines, saws and osc 2 alone: the size is the same,
                                         * the sign follows the wave's value at the first frame; with an init phase
                                         * that is always the same sign). So with a saturation stage it mostly
                                         * drives further into the curve and is taken off there. */
                float x0 = b->tk ? xt0 + xo[0] - xt[0] : 0.f;   /* (the ticks' waves at the first frame, the sub and osc 3 as they are) */
                for (i = 0; i < SY_BLOCK && x0 == 0.f; i++) if (xo[i] > 1e-6f || xo[i] < -1e-6f) x0 = xo[i];
                v->click_sg = x0 < 0.f ? -1.f : 1.f;
            }
            g *= v->click_sg;
            for (i = 0; i < SY_BLOCK && t < (float)(CLKTAB_N - 1); i++) {
                int q = (int)t;
                xo[i] += g * (CLKTAB[q] + (t - (float)q) * (CLKTAB[q + 1] - CLKTAB[q]));
                t += r;
            }
            v->click = t;
        }
        v->lp_sub = slp;
        if (nzpost && b->noise_g == 0.f) for (i = 0; i < SY_BLOCK; i++) xn[i] = 0.f;   /* (with the noise on, every xn[i]
                                         * is written below) */
        if (b->noise_g != 0.f && !lpn_done) {   /* noise: white, or coloured by one pole */
            float nl = v->n_lp, nh = v->n_hp, nx = v->n_x;
            const float ng = b->noise_g * b->n_gain;   /* (the expression's first factors, once: the same value) */
            /* (CLEANER, 1.18C25: the generator's state, the mode, the coefficients and the target in locals: the same
             * values) */
            const float lpa = b->n_lpa, lpb = b->n_lpb, hpa = b->n_hpa;
            const int nm = b->n_mode;
            uint32_t rs = v->rng;
            const int f1x = b->f1x != 0.f, st = nzpost || f1x;   /* (F1_STALE: the noise into xn, added to xo
                                         * after the loop: filter 1's mode 4 / 6 reads it there) */
            float *restrict nd = st ? xn : split ? xo2 : xo;
#if P3_NZ
            if (nm == 1) for (i = 0; i < SY_BLOCK; i++) {   /* (P3_NZ: g n into xn, one loop per colour; xn is free
                                         * without nzpost and f1x: vr_filt reads it only then) */
                const float n = sy_randf(&rs), yv = lpa * (n + nx) - lpb * nl;   /* bilinear: a zero at Nyquist
                                         * (osc.json noise, the top band) */
                nx = n; nl = yv;
                xn[i] = ng * yv;
            } else if (nm == 2) for (i = 0; i < SY_BLOCK; i++) {
                float n = sy_randf(&rs);
                nh += hpa * (n - nh); n -= nh;
                xn[i] = ng * n;
            } else for (i = 0; i < SY_BLOCK; i++) xn[i] = ng * sy_randf(&rs);
            if (!st || f1x) {
                if (f1x) nd = xo;
                for (i = 0; i < SY_BLOCK; i++) nd[i] += xn[i];
            }
#else
            for (i = 0; i < SY_BLOCK; i++) {
                float n = sy_randf(&rs);
                if (nm == 1) {          /* bilinear: a zero at Nyquist (osc.json noise, the top band) */
                    float yv = lpa * (n + nx) - lpb * nl;
                    nx = n; nl = yv; n = yv;
                }
                else if (nm == 2) { nh += hpa * (n - nh); n -= nh; }
                if (st) nd[i] = ng * n; else nd[i] += ng * n;
            }
            if (f1x) for (i = 0; i < SY_BLOCK; i++) xo[i] += xn[i];
#endif
            v->rng = rs;
            v->n_lp = nl;
            v->n_x = nx;
            v->n_hp = nh;
        }
    }
    v->ph1 = ph1; v->ph2 = ph2; v->ph3 = ph3; v->phs = phs;
}

#ifndef FILT_PASS
#define FILT_PASS 1                     /* CLEANER (cpu-filt): the digital filters as passes of one loop set (filt_svf):
                                         * filter 1, its second stage (serial 6) and filter 2 each a pass over the block;
                                         * filter 2's input and the balance's mix only where they are not the identity;
                                         * the saturation's pre- / post-scale inside the stage's loops. The same
                                         * operations in the same order: bit-identical (but the signs of zeros).
                                         * 0: the loops of 1.18C34 */
#endif
#if FILT_PASS
/* one state-variable filter pass over a block: out[i] = Y(in[i] x pin), Y by the mode (0 low, 1 high, 2 wb x band,
 * 3 wb x band + input: the notch / peak mix with the weights 0 of the other terms left out: the same values).
 * The coefficients: g from g0 by dg a sample (dg 0: constant), h = 1 / (1 + (k + g) g), exact at the block's start
 * (here) and by one Newton step a sample while g moves, k constant. A caller may run any start / step through it (a
 * smoothed cutoff: g0, dg per block); st: the two integrators. in may be out (sample by sample). m + SVF_TWO
 * (FILT_TWO): a second stage of the same coefficients on the first's output (serial 6), its integrators at st + 2
 * (sy_voice_t: f1, f2a, f2b), in the same loop (the step's work once for both); for modes 0-2 while g moves only,
 * else the caller runs the second stage as a pass of its own. (No sixth argument: the caller's frame) */
typedef struct { float g, dg, k, pin, wb; } filt_svfk;
#ifndef FILT_TWO
#define FILT_TWO 1
#endif
#define SVF_TWO 8
#ifndef FILT_YS
#define FILT_YS 1                       /* (vr_filt's block in yl / yr, not on the stack; 0: on the stack) */
#endif
static SY_HOT __attribute__((noinline)) void filt_svf(const float *in, float *out, sy_svf_t *st, const filt_svfk *c, int m)
{
    int i;
    float g = c->g, s1 = st->s1, s2 = st->s2;
    const float dg = c->dg, k = c->k, pin = c->pin, wb = c->wb;
    float kg = k + g, h = 1.f / (1.f + (k + g) * g);   /* (as vr_filt's: the same value) */
#define SVFP_LOOP(Y, RAMP) \
    for (i = 0; i < SY_BLOCK; i++) { \
        float xi = in[i] * pin, hp, bp, lp; \
        if (RAMP) { \
            g += dg; \
            kg = k + g; \
            h = h * (2.f - h * (1.f + kg * g)); \
        } \
        hp = (xi - kg * s1 - s2) * h; \
        bp = g * hp + s1; \
        s1 = g * hp + bp; \
        lp = g * bp + s2; \
        s2 = g * bp + lp; \
        out[i] = Y; \
    }
#if FILT_TWO
#define SVFP2_LOOP(Y) \
    for (i = 0; i < SY_BLOCK; i++) { \
        float xi = in[i] * pin, hp, bp, lp; \
        g += dg; \
        kg = k + g; \
        h = h * (2.f - h * (1.f + kg * g)); \
        hp = (xi - kg * s1 - s2) * h; \
        bp = g * hp + s1; \
        s1 = g * hp + bp; \
        lp = g * bp + s2; \
        s2 = g * bp + lp; \
        xi = Y; \
        hp = (xi - kg * c1 - c2) * h; \
        bp = g * hp + c1; \
        c1 = g * hp + bp; \
        lp = g * bp + c2; \
        c2 = g * bp + lp; \
        out[i] = Y; \
    }
    if (m & SVF_TWO) {                 /* (the caller's test: modes 0-2, dg not 0) */
        sy_svf_t *st2 = st + 2;
        float c1 = st2->s1, c2 = st2->s2;
        m -= SVF_TWO;
        if (m == 0) SVFP2_LOOP(lp)
        else if (m == 1) SVFP2_LOOP(hp)
        else SVFP2_LOOP(wb * bp)
        st2->s1 = c1; st2->s2 = c2;
        st->s1 = s1; st->s2 = s2;
        return;
    }
#undef SVFP2_LOOP
#endif
    if (dg != 0.f) {
        if (m == 0) SVFP_LOOP(lp, 1)
        else if (m == 1) SVFP_LOOP(hp, 1)
        else if (m == 2) SVFP_LOOP(wb * bp, 1)
        else SVFP_LOOP(wb * bp + xi, 1)
    } else {
        if (m == 0) SVFP_LOOP(lp, 0)
        else if (m == 1) SVFP_LOOP(hp, 0)
        else if (m == 2) SVFP_LOOP(wb * bp, 0)
        else SVFP_LOOP(wb * bp + xi, 0)
    }
#undef SVFP_LOOP
    st->s1 = s1; st->s2 = s2;
}
#endif

static SY_HOT __attribute__((noinline)) void vr_filt(sy_voice_t *v, const sy_vblk_t *b, const float *xo, const float *xo2,
                                              const float *xn, int nzpost, int split, float *yl, float *yr, int *stereo_out,
                                              float *restrict xq)   /* xq: a block's scratch (filter 2's input) */
{
    int i, stereo = 0;
    const float inv = 1.f / SY_BLOCK;

    /* -- filter 1, the saturation, filter 2, the routing: one pass each, the mode decisions outside the loops */
    {
        float g = v->g1, h = v->h1, dg = (b->g1e - g) * inv;
        float g2 = v->g2, h2 = v->h2, dg2 = (b->g2e - g2) * inv;
        float k = b->k1, pg = b->pg1, k2 = b->k2, pg2 = b->pg2;
        float s1 = v->f1.s1, s2 = v->f1.s2;
        float a1 = v->f2a.s1, a2 = v->f2a.s2, c1 = v->f2b.s1, c2 = v->f2b.s2;
#if FILT_PASS && FILT_YS
        /* filter 1's and the saturation's block in an output buffer of the voice's work (no 512 B on the stack):
         * mono in yr (vr_amp reads yr only in stereo), split (stereo) in yl (its mix scales it in place), and in yl
         * wherever filter 2 is off and filter 1's share is 1 (serial at balance 0: yl = 1 x y is y, no copy) */
        const int ydir = b->route == 3 || (b->bal_b == 0.f && b->bal_a == 1.f);
        float *restrict const ys = ydir ? yl : yr;
#else
        float ys[SY_BLOCK];             /* (local: through a pointer the loops reload the block's values) */
#endif
#if FILT_PASS
        filt_svfk c;                        /* (one for filter 1 and 2: the frame) */
#endif
        int m1 = b->m1, m2 = b->m2, f2on = b->bal_b != 0.f, sat = b->sat;
        int sat_pre = SAT_VPRE != 1.f && sat >= SAT_VPOST_MIN && sat <= 10 && !b->use_input && !(m1 == 5 || m1 == 7);
        int pre_done = 0;               /* (the pre-scale done in filter 1's store) */
        float post;                     /* (the post-scale, by the readers of ys after the stage) */
        h = 1.f / (1.f + (k + g) * g);              /* exact at the block's start (the resonance may have moved) */
        if (f2on) h2 = 1.f / (1.f + (k2 + g2) * g2);   /* (read in filter 2's loop alone) */
        stereo = b->route == 3;
        if (m1 == 5 || m1 == 7) {       /* analog: the ladder loop (synth.c), the stages' coefficient gliding
                                         * over the block */
            float z0 = v->lad[0], z1 = v->lad[1], z2 = v->lad[2], z3 = v->lad[3];
            float a = v->lad_a > 0.f ? v->lad_a : b->lad_a, da = (b->lad_a - a) * inv;
            float k = b->lad_k, gin = b->lad_gin, go = b->lad_gout, lc = b->lad_c, ic = 1.f / b->lad_c;
#if LAD_STS_ON
            float ss = b->lad_ss, sk = LADST_K / ss;
            const float ss1 = b->lad_ss1, sk1 = LADST_K / ss1;   /* (stage 2's input: the limiter at high resonance) */
#endif
            uint32_t nrs = v->lad_rng;
            const float gnz = gin * LAD_NZ_G;   /* (the product's first factors, once: the same value) */
            for (i = 0; i < SY_BLOCK; i++) {
                float u = (gin * xo[i] - k * z3) * ic, w = sy_absf(u), x;
                int q;
                a += da;
                w = w / (1.f + w) * (float)LADSAT_N;            /* S(u) from the table over |u| / (1 + |u|) */
                q = (int)w;
                if (q >= LADSAT_N) q = LADSAT_N - 1;
                x = LADSAT[q] + (w - (float)q) * (LADSAT[q + 1] - LADSAT[q]);
                u = u < 0.f ? -lc * x : lc * x;
                if (nzpost) u += gnz * xn[i];
#if LAD_NZ_MODE == 1
                if (m1 == 7) { nrs = nrs * 1664525u + 1013904223u; u += (float)(int32_t)nrs * (LAD_NZ_A / 2147483648.f); }
#endif
                z0 += a * (u - z0);
#if LAD_STS_ON
                z1 += a * (lad_st(z0, sk1, ss1) - z1);
                z2 += a * (lad_st(z1, sk, ss) - z2);
                z3 += a * (lad_st(z2, sk, ss) - z3);
#else
                z1 += a * (z0 - z1);
                z2 += a * (z1 - z2);
                z3 += a * (z2 - z3);
#endif
                if (m1 == 7) {          /* mode 7's own noise, into the last stage (the reference's 4-pole
                                         * mode hisses at -97 dBFS while a note sounds, its spectrum following the
                                         * cutoff: read without input; uniform +-2.3e-4) */
                    nrs = nrs * 1664525u + 1013904223u;
#if LAD_NZ_MODE == 0
                    z3 += (float)(int32_t)nrs * (LAD_NZ_A / 2147483648.f) * b->lad_nz;
#elif LAD_NZ_MODE == 2
                    z3 += a * (float)(int32_t)nrs * (LAD_NZ_A / 2147483648.f);
#elif LAD_NZ_MODE == 3
                    { float nn = a * (float)(int32_t)nrs * (LAD_NZ_A / 2147483648.f); z0 += nn; }
#elif LAD_NZ_MODE == 9          /* (the search: stage LAD_NZ_ST, scaled by a^LAD_NZ_P) */
                    {   float nn = (float)(int32_t)nrs * (LAD_NZ_A / 2147483648.f), sc = 1.f; int pp;
                        for (pp = 0; pp < LAD_NZ_P; pp++) sc *= a;
                        nn *= sc;
                        if (LAD_NZ_ST == 0) z0 += nn; else if (LAD_NZ_ST == 1) z1 += nn; else if (LAD_NZ_ST == 2) z2 += nn; else z3 += nn; }
#endif
                }
                ys[i] = go * (m1 == 5 ? z1 : z3);
            }
            v->lad_a = b->lad_a;
            v->lad_rng = nrs;
            v->lad[0] = z0; v->lad[1] = z1; v->lad[2] = z2; v->lad[3] = z3;
        } else if (b->f1x != 0.f) {     /* CLEANER (F1_STALE, synth.c): mode 4 / 6 with the coloured noise: the noise
                                         * alone, flat */
            const float gx = b->f1x / b->noise_g;   /* (xn: the noise at its volume, noise_g x n_gain) */
            for (i = 0; i < SY_BLOCK; i++) ys[i] = gx * xn[i];
        } else if (m1 == 6 || m1 == 4) {
            float gm = m1 == 4 ? b->m4_g : 1.f;     /* 4: -25 dB in the static path, silence in a voice (filt.json) */
            for (i = 0; i < SY_BLOCK; i++) ys[i] = xo[i] * gm;
        } else {                        /* the digital state-variable filter; the band stop is not under the passband
                                         * loss, the band pass grows with the resonance (filt.json) */
#if FILT_PASS
            /* serial 6, filter 1 twice also in a voice (see below): the second stage a pass of its own over
             * the first's output (no feedback between them; the same g and h a sample: the pass runs the same
             * steps again from the block's start) */
            c.g = g; c.dg = dg; c.k = k; c.pin = m1 == 3 ? 1.f : pg;
            c.wb = m1 == 2 ? b->bp1 : -k;
            {
                const int two = b->route == 1, both = FILT_TWO && two && dg != 0.f && m1 <= 2;
                const float *xf = xo;   /* (the pass's input) */
#if FS_PG
                if (v->pr && c.pin != v->pg1) {   /* CLEANER (FILT KNOBS SMOOTH, synth.c flt_glide): the passband gain
                                         * from the last block's across this one (in xq: filter 2's input comes later;
                                         * x f, then x pin in the pass: the products of the loops of FILT_PASS 0) */
                    const float d = (1.f - v->pg1 / c.pin) * inv;
                    float f = v->pg1 / c.pin;
                    for (i = 0; i < SY_BLOCK; i++) { f += d; xq[i] = xo[i] * f; }
                    xf = xq;
                }
                v->pg1 = c.pin;
#endif
                filt_svf(xf, ys, &v->f1, &c, both ? m1 + SVF_TWO : m1);   /* (f2b: v->f1 + 2) */
                if (two && !both) {
                    c.pin = 1.f;
                    filt_svf(ys, ys, &v->f2b, &c, m1);
                }
            }
#else
            float pin = m1 == 3 ? 1.f : pg, wl = m1 == 0, wh = m1 == 1, wb = m1 == 2 ? b->bp1 : m1 >= 3 ? -k : 0.f;
            float wx = m1 >= 3 ? 1.f : 0.f;
            int two = b->route == 1;    /* serial 6, filter 1 twice also in a voice (was: in the static path only
                                         * (filt.json routing 1: a 4-pole low pass at balance 0, filter 2 alone
                                         * 2-pole at 127); a voice in serial 6 sounds as in serial 4
                                         * (measured: within 0.5 dB and the same spectrum;
                                         * input mode 1 as the static path: ASSUMPTION) */
            /* the output's mix in four loops: low, high and band pass alone take their own output (the mix's terms of
             * weight 0 add +-0: the same values), the notch and peak the mix */
            float kg = k + g;           /* (k + g, again only when g moves: the same values, fewer adds) */
            float pre1 = sat_pre ? SAT_VPRE : 1.f;   /* (the saturation's pre-scale in the store: y * 1 is y) */
            const float *xf = xo;       /* (the loops' input) */
#if FS_PG
            if (v->pr && pin != v->pg1) {   /* CLEANER (FILT KNOBS SMOOTH, synth.c flt_glide): the passband gain from
                                         * the last block's across this one (in xq: filter 2's input comes later) */
                const float d = (1.f - v->pg1 / pin) * inv;
                float f = v->pg1 / pin;
                for (i = 0; i < SY_BLOCK; i++) { f += d; xq[i] = xo[i] * f; }
                xf = xq;
            }
            v->pg1 = pin;
#endif
            pre_done = 1;
            /* (RAMP 0: the loop without the coefficient step, for a block where g does not move: the step was not
             * taken there anyway, so the same values) */
#define SVF_LOOP(Y1, Y2, RAMP) \
            for (i = 0; i < SY_BLOCK; i++) { \
                float xi = xf[i] * pin, hp, bp, lp, y; \
                if (RAMP && dg != 0.f) { /* h = 1/(1 + kg + g^2): one Newton step per sample while g moves */ \
                    g += dg; \
                    kg = k + g; \
                    h = h * (2.f - h * (1.f + kg * g)); \
                } \
                hp = (xi - kg * s1 - s2) * h; \
                bp = g * hp + s1; \
                s1 = g * hp + bp; \
                lp = g * bp + s2; \
                s2 = g * bp + lp; \
                y = Y1; \
                if (two) { \
                    float xj = y; \
                    hp = (xj - kg * c1 - c2) * h; \
                    bp = g * hp + c1; \
                    c1 = g * hp + bp; \
                    lp = g * bp + c2; \
                    c2 = g * bp + lp; \
                    y = Y2; \
                } \
                ys[i] = y * pre1; \
            }
#define SVF_LOOP2(Y1, Y2) { if (dg != 0.f) SVF_LOOP(Y1, Y2, 1) else SVF_LOOP(Y1, Y2, 0) }
            if (m1 == 0) SVF_LOOP2(lp, lp)
            else if (m1 == 1) SVF_LOOP2(hp, hp)
            else if (m1 == 2) SVF_LOOP2(wb * bp, wb * bp)
            else SVF_LOOP2(wl * lp + wh * hp + wb * bp + wx * xi, wl * lp + wh * hp + wb * bp + wx * xj)
#undef SVF_LOOP2
#undef SVF_LOOP
#endif
        }
        if (m1 == 5 || m1 == 7) sat = 0;   /* the analog modes bypass the saturation stage (read: every type 1-14
                                         * leaves a sine through mode 5 or 7 sample-identical, at osc volume 32-110) */
#if FILT_PASS
        /* (the pre-scale on the stage's input and the post-scale on its output inside its loops: the same products) */
        const float vp = sat_pre ? SAT_VPRE : 1.f, vq = sat_pre ? 1.f / SAT_VPRE : 1.f;
#define SAT_IN(x) ((x) * vp)
#define SAT_OUT(x) ((x) * vq)
        (void)pre_done;
        if (0)
#else
#define SAT_IN(x) (x)
#define SAT_OUT(x) (x)
        if (sat_pre && !pre_done)
#endif
                                        /* in a voice the stage sees 3.8 % less
                                         * than the static path's calibration gives it, and the stage's output is
                                         * raised as much again (read: the shaper's period in a voice, drive 64-127:
                                         * a constant ratio 0.9623; linear types do not see it; types 1-4 read
                                         * best without it in the test bank's patches, their curves were read on a
                                         * voice) */
            for (i = 0; i < SY_BLOCK; i++) ys[i] *= SAT_VPRE;
        switch (sat) {                  /* the saturation stage, right after filter 1 */
        case 1: case 2: case 3: case 4:
#if FILT_PASS
        {   /* sat_curve: t = (x / R + 1) N / 2 as x (N / 2R) + N / 2 (R and N powers of 2: the scalings are exact and
             * commute with the rounding of the add: the same t); the point's two values through two bases (no
             * address add) */
#ifdef HYB_MULTI
            const float *l0 = b->sat_lut;
#else
            const float *l0 = sy_sat_lut;
#endif
            const float *l1 = l0 + 1, lo = l0[0], hi = l0[SAT_N];
            for (i = 0; i < SY_BLOCK; i++) {
                const float t = ys[i] * (SAT_N * 0.5f / SAT_RANGE) + SAT_N * 0.5f;
                if (__builtin_expect(t > 0.f && t < (float)SAT_N, 1)) {
                    const int q = (int)t;
                    const float a = l0[q];
                    ys[i] = a + (t - (float)q) * (l1[q] - a);
                } else ys[i] = t <= 0.f ? lo : hi;
            }
        }
#else
            for (i = 0; i < SY_BLOCK; i++) ys[i] = sat_curve(ys[i]);
#endif
            break;
        case 5: {                       /* a hard clip, slope 1.66 at drive 64 (sat.json static_curve 5),
                                         * steeper with the drive */
            float hb = b->sat_b, g = b->sat_g5;
            for (i = 0; i < SY_BLOCK; i++) ys[i] = SAT_OUT(sy_clampf(g * SAT_IN(ys[i]), -hb, hb));
            break;
        }
        case 6: for (i = 0; i < SY_BLOCK; i++) ys[i] = SAT_OUT(b->sat_b * SIN_CYC(SAT_IN(ys[i]) * b->shaper_k)); break;
        case 7: {                       /* y = HP(r(clip(g x))), r(u) = u above 0 and m u below, the clip at
                                         * +-1.174, the high pass a one-pole at 19.7 Hz (read with the high pass taken
                                         * out again: a sine through the static path, drive 64-127 in steps of 8; g
                                         * rises linearly to 2.0 at 127, m falls from 1 to -1: a full rectifier) */
            float slp = v->sat_dc, g7 = b->sat_a7, m7 = b->sat_b7, c = b->sat_a;
            for (i = 0; i < SY_BLOCK; i++) {
                float u = sy_clampf(g7 * SAT_IN(ys[i]), -1.1736f, 1.1736f), z = u > 0.f ? u : m7 * u;
                slp += c * (z - slp);
                ys[i] = SAT_OUT(z - slp);
            }
            v->sat_dc = slp;
            break;
        }
        case 8: {
            float q = b->sat_a, cnt = v->sat_cnt, hold = v->sat_hold;
#ifdef HYB_FAST_VOICE
            const float iq = 1.f / q;
#endif
            if (cnt < 0.f) cnt = b->sat_c - 1.f;     /* a new note takes its first sample at once */
#if FILT_PASS && !defined(HYB_FAST_VOICE)
            if (b->sat_c == 1.f && cnt >= 0.f && cnt < 1e6f && cnt == (float)(int)cnt) {
                /* (synth.c: sat_c is 1: every sample holds; cnt + 1 - 1 is cnt for a whole number, so the counter
                 * stays; the floor in floats: (float)(n - 1) is (float)n - 1 for |n| < 2^24: the same values) */
                for (i = 0; i < SY_BLOCK; i++) {
                    const float z = SAT_IN(ys[i]) / q + 0.5f;
                    float f = (float)(int)z;
                    if (z < f) f -= 1.f;
                    hold = f * q;
                    ys[i] = SAT_OUT(hold);
                }
                v->sat_cnt = cnt; v->sat_hold = hold;
                break;
            }
#endif
            for (i = 0; i < SY_BLOCK; i++) {
                cnt += 1.f;
#ifdef HYB_FAST_VOICE
                if (cnt >= b->sat_c) { cnt -= b->sat_c; hold = (float)sy_floori(SAT_IN(ys[i]) * iq + 0.5f) * q; }
#else
                if (cnt >= b->sat_c) { cnt -= b->sat_c; hold = (float)sy_floori(SAT_IN(ys[i]) / q + 0.5f) * q; }
#endif
                ys[i] = SAT_OUT(hold);
            }
            v->sat_cnt = cnt; v->sat_hold = hold;
            break;
        }
        case 9: case 10: {
            float cnt = v->sat_cnt, hold = v->sat_hold;
            if (cnt < 0.f) cnt = b->sat_a - 1.f;     /* a new note takes its first sample at once */
            for (i = 0; i < SY_BLOCK; i++) {
                cnt += 1.f;
                if (cnt >= b->sat_a) { cnt -= b->sat_a; hold = SAT_IN(ys[i]); }
                ys[i] = SAT_OUT(hold);
            }
            v->sat_cnt = cnt; v->sat_hold = hold;
            break;
        }
        case 11: case 12: {
            float slp = v->sat_lp, sa = b->sat_a;
            for (i = 0; i < SY_BLOCK; i++) { slp += sa * (ys[i] - slp); ys[i] = slp; }
            v->sat_lp = slp;
            break;
        }
        case 13: case 14: {             /* a bilinear one-pole high pass (read: unity at the top even with its
                                         * corner near Nyquist; x - lowpass lost up to 15 dB there) */
            float y1 = v->sat_lp, x1 = v->sat_dc, b0 = b->sat_a, a1 = b->sat_b;
#if FILT_PASS
            for (i = 0; i < SY_BLOCK; i += 2) {   /* (two samples a turn: no register move for x1) */
                const float x = ys[i];
                y1 = b0 * (x - x1) + a1 * y1;
                ys[i] = y1;
                x1 = ys[i + 1];
                y1 = b0 * (x1 - x) + a1 * y1;
                ys[i + 1] = y1;
            }
#else
            for (i = 0; i < SY_BLOCK; i++) { float x = ys[i]; y1 = b0 * (x - x1) + a1 * y1; x1 = x; ys[i] = y1; }
#endif
            v->sat_lp = y1; v->sat_dc = x1;
            break;
        }
        default: break;
        }
#undef SAT_IN
#undef SAT_OUT
#if FILT_PASS
        post = 1.f;                     /* (done in the stage's loops) */
        (void)post; (void)h; (void)h2; (void)s1; (void)s2; (void)a1; (void)a2; (void)c1; (void)c2;   /* (the old
                                         * path's: filt_svf keeps h and the states) */
#else
        post = sat_pre ? 1.f / SAT_VPRE : 1.f;   /* (the stage's output raised as much again: by its readers below) */
#endif
        if (f2on) {                     /* filter 2: modes 0-3 (4 and up: band stop) */
#if FILT_PASS
            const float bc = b->bal_c, ba = b->bal_a, bb = b->bal_b, bc1 = 1.f - bc;
            const float *fin;
            c.g = g2; c.dg = dg2; c.k = k2; c.pin = m2 == 3 ? 1.f : pg2;
            c.wb = m2 == 2 ? b->bp2 : -k2;
            /* the input (bc x the oscillators + (1 - bc) x filter 1) x pin, pin in the pass; where one weight is 0 and
             * the other 1 the term alone (0 x a + 1 x b is b): serial up to balance 64 filter 1's output, parallel and
             * serial at 127 the oscillators */
            if (split) fin = xo2;
            else if (!nzpost && bc == 0.f) fin = ys;
            else if (!nzpost && bc == 1.f) fin = xo;
            else {
                if (nzpost) for (i = 0; i < SY_BLOCK; i++) xq[i] = bc * (xo[i] + xn[i]) + bc1 * ys[i];
                else for (i = 0; i < SY_BLOCK; i++) xq[i] = bc * xo[i] + bc1 * ys[i];
                fin = xq;
            }
#if FS_PG
            if (v->pr && c.pin != v->pg2) {   /* (FILT KNOBS SMOOTH: as filter 1's; x pin, then x f as FILT_PASS 0's
                                         * loops, the pass's pin 1: x 1 is the same value) */
                const float pin = c.pin, d = (1.f - v->pg2 / pin) * inv;
                float f = v->pg2 / pin;
                for (i = 0; i < SY_BLOCK; i++) { f += d; xq[i] = fin[i] * pin * f; }
                fin = xq;
                c.pin = 1.f;
            }
            v->pg2 = m2 == 3 ? 1.f : pg2;
#endif
            /* the output: filter 2 alone straight into yl (0 x y + 1 x y2 is y2: serial at balance 64 and up), else the
             * balance's mix in a pass of its own */
            if (!stereo && ba == 0.f && bb == 1.f) filt_svf(fin, yl, &v->f2a, &c, m2);
            else {
                filt_svf(fin, xq, &v->f2a, &c, m2);
#if FILT_YS
                if (stereo) for (i = 0; i < SY_BLOCK; i++) { ys[i] = ba * ys[i]; yr[i] = bb * xq[i]; }   /* (ys: yl) */
#else
                if (stereo) for (i = 0; i < SY_BLOCK; i++) { yl[i] = ba * ys[i]; yr[i] = bb * xq[i]; }
#endif
                else for (i = 0; i < SY_BLOCK; i++) yl[i] = ba * ys[i] + bb * xq[i];
            }
#else
            float pin = m2 == 3 ? 1.f : pg2, wl = m2 == 0, wh = m2 == 1, wb = m2 == 2 ? b->bp2 : m2 >= 3 ? -k2 : 0.f;
            float wx = m2 >= 3 ? 1.f : 0.f, bc = b->bal_c, ba = b->bal_a, bb = b->bal_b;
            float kg2 = k2 + g2, bc1 = 1.f - bc;
            /* the input in a pass of its own (the decisions out of the filter's loop); ys raised by post here */
            if (split) for (i = 0; i < SY_BLOCK; i++) { ys[i] *= post; xq[i] = xo2[i] * pin; }
            else if (nzpost) for (i = 0; i < SY_BLOCK; i++) { float y = ys[i] * post; ys[i] = y; xq[i] = (bc * (xo[i] + xn[i]) + bc1 * y) * pin; }
            else for (i = 0; i < SY_BLOCK; i++) { float y = ys[i] * post; ys[i] = y; xq[i] = (bc * xo[i] + bc1 * y) * pin; }
#if FS_PG
            if (v->pr && pin != v->pg2) {   /* (FILT KNOBS SMOOTH: as filter 1's) */
                const float d = (1.f - v->pg2 / pin) * inv;
                float f = v->pg2 / pin;
                for (i = 0; i < SY_BLOCK; i++) { f += d; xq[i] *= f; }
            }
            v->pg2 = pin;
#endif
            /* (RAMP 0: the loop without the coefficient step, for a block where g2 does not move: the step was
             * not taken there anyway) */
#define SVF2_LOOP(Y2, RAMP) \
            for (i = 0; i < SY_BLOCK; i++) { \
                float y = ys[i], xi = xq[i], hp, bp, lp, y2; \
                if (RAMP && dg2 != 0.f) { \
                    g2 += dg2; \
                    kg2 = k2 + g2; \
                    h2 = h2 * (2.f - h2 * (1.f + kg2 * g2)); \
                } \
                hp = (xi - kg2 * a1 - a2) * h2; \
                bp = g2 * hp + a1; \
                a1 = g2 * hp + bp; \
                lp = g2 * bp + a2; \
                a2 = g2 * bp + lp; \
                y2 = Y2; \
                if (stereo) { yl[i] = ba * y; yr[i] = bb * y2; } \
                else yl[i] = ba * y + bb * y2; \
            }
#define SVF2_LOOP2(Y2) { if (dg2 != 0.f) SVF2_LOOP(Y2, 1) else SVF2_LOOP(Y2, 0) }
            if (m2 == 0) SVF2_LOOP2(lp)         /* (as filter 1: a plain mode takes its own output) */
            else if (m2 == 1) SVF2_LOOP2(hp)
            else if (m2 == 2) SVF2_LOOP2(wb * bp)
            else SVF2_LOOP2(wl * lp + wh * hp + wb * bp + wx * xi)
#undef SVF2_LOOP2
#undef SVF2_LOOP
#endif
        } else {
            float ba = b->bal_a;
#if FILT_PASS && FILT_YS
            if (!ydir) for (i = 0; i < SY_BLOCK; i++) yl[i] = ba * ys[i];   /* (post 1 here: y x 1 is y) */
            else if (stereo) for (i = 0; i < SY_BLOCK; i++) ys[i] = ba * ys[i];   /* (ys: yl; else ba is 1) */
#elif FILT_PASS
            for (i = 0; i < SY_BLOCK; i++) yl[i] = ba * ys[i];   /* (post 1 here: y x 1 is y) */
#else
            for (i = 0; i < SY_BLOCK; i++) yl[i] = ba * (ys[i] * post);
#endif
            if (stereo) for (i = 0; i < SY_BLOCK; i++) yr[i] = 0.f;
        }
#if !FILT_PASS
        v->f1.s1 = s1; v->f1.s2 = s2; v->f2a.s1 = a1; v->f2a.s2 = a2; v->f2b.s1 = c1; v->f2b.s2 = c2;
#endif
        v->g1 = b->g1e; v->h1 = b->h1e; v->g2 = b->g2e; v->h2 = b->h2e;
    }

    *stereo_out = stereo;
}

static SY_HOT __attribute__((noinline)) void vr_amp(sy_voice_t *v, const sy_vblk_t *b, const float *restrict yl, const float *restrict yr, int stereo,
                                             float *restrict out_l, float *restrict out_r)
{
    int i;

    /* -- the amp and the pan, interpolated per sample (in two segments when the attack ends inside the block) */
    {
        float gl = v->gl, gr = v->gr, dl, dr;
        int k = b->knee < SY_BLOCK ? b->knee : SY_BLOCK, seg;
        for (seg = 0; seg < 2; seg++) {
            int i0 = seg ? k : 0, i1 = seg ? SY_BLOCK : k;
            float el = seg || k == SY_BLOCK ? b->gle : b->gkl, er = seg || k == SY_BLOCK ? b->gre : b->gkr;
            if (i1 <= i0) { if (!seg && k == 0) { gl = b->gkl; gr = b->gkr; } continue; }
            if (!seg && b->atk) {       /* the attack's curve sample by sample (sy_atk_curve) */
                /* 0.93: its level (amp x pan without the envelope) from the last block's (v->afl, v->afr) to this
                 * block's in steps per sample: a moving pan or volume (an LFO through the matrix) glides; unmoved, the
                 * step is 0 and the level stays as it is (the same bits) */
                const float *yq = stereo ? yr : yl, l0 = b->al0, len = b->alen, an0 = b->an0,
                            dfl = (b->afl - v->afl) / (float)(i1 - i0), dfr = (b->afr - v->afr) / (float)(i1 - i0);
                float afl = v->afl, afr = v->afr;
                if (len < 5.f)
                    for (i = i0; i < i1; i++) {
                        float e = sy_atk_curve(an0 + (float)i, l0, len);
                        afl += dfl; afr += dfr;
                        out_l[i] += afl * e * yl[i];
                        out_r[i] += afr * e * yq[i];
                    }
                else {                  /* sy_atk_curve as it is, its grid points (the two divisions) only where the
                                         * grid point changes: the same operands, so the same values */
                    const int six = len < 6.f;
                    float kp = -1.f, c0 = 0.f, c1 = 0.f;
#ifdef HYB_FAST_VOICE
                    const float ilen = 1.f / len;
#endif
                    for (i = i0; i < i1; i++) {
                        float n = an0 + (float)i, k, e;
                        if (six) n += 1.f;
                        k = (float)(int)(n * 0.125f) * 8.f;
                        if (k != kp) {
                            kp = k;
#ifdef HYB_FAST_VOICE
                            c0 = l0 + k * ilen; c1 = l0 + (k + 8.f) * ilen;
#else
                            c0 = l0 + k / len; c1 = l0 + (k + 8.f) / len;
#endif
                            if (c0 > 1.f) c0 = 1.f;
                            if (c1 > 1.f) c1 = 1.f;
                        }
                        e = c0 + (c1 - c0) * (n - k) * 0.125f;
                        afl += dfl; afr += dfr;
                        out_l[i] += afl * e * yl[i];
                        out_r[i] += afr * e * yq[i];
                    }
                }
                gl = el; gr = er;
                v->afl = b->afl; v->afr = b->afr;
                if (k == SY_BLOCK) break;
                continue;
            }
            dl = (el - gl) / (float)(i1 - i0);
            dr = (er - gr) / (float)(i1 - i0);
            if (b->expo && seg == (k < SY_BLOCK) && gl * el > 1e-20f && gr * er > 1e-20f) {
                /* decay and release: straight in dB inside the block too (env.json: the decay curves) */
                float ql = qroot(el / gl, i1 - i0), qr = qroot(er / gr, i1 - i0);
                if (stereo)
                    for (i = i0; i < i1; i++) { gl *= ql; gr *= qr; out_l[i] += gl * yl[i]; out_r[i] += gr * yr[i]; }
                else
                    for (i = i0; i < i1; i++) { gl *= ql; gr *= qr; out_l[i] += gl * yl[i]; out_r[i] += gr * yl[i]; }
                if (k == SY_BLOCK) break;
                continue;
            }
            if (stereo)
                for (i = i0; i < i1; i++) { gl += dl; gr += dr; out_l[i] += gl * yl[i]; out_r[i] += gr * yr[i]; }
            else
                for (i = i0; i < i1; i++) { gl += dl; gr += dr; out_l[i] += gl * yl[i]; out_r[i] += gr * yl[i]; }
            if (k == SY_BLOCK) break;
        }
        v->gl = b->gle;
        v->gr = b->gre;
    }
}

SY_HOT void sy_voice_render_w(sy_synth *s, sy_voice_t *v, const sy_vblk_t *b, float *out_l, float *out_r, const float *in, int core)
{
    sy_vwork_t *w = &sy_work.vw[core];
    int nzpost = LAD_NZ_POST && (b->m1 == 5 || b->m1 == 7) && b->route != 3;   /* noise past the analog loop's input stage
                                         * (filter 2 takes it as usual) */
    int split = b->route == 3;     /* split: osc 1 + sub into filter 1 (left), osc 2 + noise into filter 2 (right) */
    int stereo;
#ifdef SY_PROF
    uint32_t t0 = core == 0 && sy_prof_clock ? sy_prof_clock() : 0u, t1, t2;   /* (the first core's: its stages' ticks) */
#endif
    vr_osc(v, b, in, w, nzpost, split);
#ifdef SY_PROF
    t1 = core == 0 && sy_prof_clock ? sy_prof_clock() : 0u;
#endif
    vr_filt(v, b, w->xo, w->xo2, w->xn, nzpost, split, w->yl, w->yr, &stereo, w->xt);   /* (the buffers one by one: the
                                         * loops keep their pointers) */
#ifdef SY_PROF
    t2 = core == 0 && sy_prof_clock ? sy_prof_clock() : 0u;
#endif
    vr_amp(v, b, w->yl, w->yr, stereo, out_l, out_r);
#ifdef SY_PROF
    if (core == 0 && sy_prof_clock) {
        sy_prof_v[0] += t1 - t0;
        sy_prof_v[1] += t2 - t1;
        sy_prof_v[2] += sy_prof_clock() - t2;
    }
#endif
}

void sy_voice_render(sy_synth *s, sy_voice_t *v, const sy_vblk_t *b, float *out_l, float *out_r, const float *in)
{
    SY_FAR(sy_voice_render_w)(s, v, b, out_l, out_r, in, 0);
}
