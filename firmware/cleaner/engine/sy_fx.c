/* SPDX-License-Identifier: GPL-3.0-only */
/* The part's effects, once per part (effects.md, addendum-1.md): distortion -> chorus -> delay
 * or reverb -> phaser -> EQ -> boost. Each runs only while its mix or send is above 0. All linear but the
 * distortion. */
#include "sy_int.h"
#ifdef SY_FX_RAM
#define SY_FXR __attribute__((section(".ram_text")))   /* (the effects' code in RAM, a firmware's choice: on the FM-1 a
                                         * cold line of the flash costs ~500 cycles; the calls into the flash go through
                                         * SY_FAR) */
#else
#define SY_FXR
#endif
/* the per-block helpers out of line: inlined into sy_fx_render (one function, the target's 16 registers) each costs
 * 30-200 bytes of spill code; a call once per block is nothing */
#define SY_FXCOLD SY_FXR static __attribute__((noinline))

#define DL_SCALE 8192.f                 /* int16 delay storage: +-4 full scale */
/* The loop's rounding below DL_DEADBAND steps (2^-7 full scale, -42 dBFS) is toward zero. Rounded to the nearest, a
 * loop whose gain g is below 1 holds any value of k <= 0.5 / (1 - g) steps for good (round(g k) = k: a dead band;
 * A013, feedback 80, g -0.625: +-1 step, -76 dBFS on the output for ~30 s after the tail, the effects never at rest
 * meanwhile; g 127/128: up to 64 steps). Toward zero |q(g k)| < k: the tail decays to exact zeros at the loop's own
 * rate and the chain rests (SY_FX_REST) ~1.5 s later (the loop rewritten once). The change is below a step (-78 dBFS
 * full scale) and only where the loop is quiet; above it the rounding is the nearest, as before */
#define DL_DEADBAND 64.f
#define DL_ISCALE (1.f / 8192.f)
#define DL_MASK (SY_DELAY_FRAMES - 1)
/* the chorus's and the delay's write positions (ch_w, dl_w): their reads take the position in float ((float)w - d,
 * rd_frac, rd16), so a position that only grows loses the fraction as it grows: 1/128 of a frame after 1.4 s, none at
 * 2^23 frames (3 minutes of the effect running), every other frame read twice or skipped at 2^24 (6 minutes; the
 * chorus's and the delay's read noise -88 / -76 dB at the start, -25 / -21 dB after 6 minutes, -19 dB after 18 with a
 * 1 kHz sine: a tinny, metallic sound until a restart). Kept within one binade, [FX_POS_LO, 2 FX_POS_LO): FX_POS_LO
 * taken off when a block would reach 2 FX_POS_LO (a multiple of every buffer's length: the same indices); up to there
 * the same values as before, after it the resolution of 2.8-5.6 s (1/64 of a frame) for good */
#define FX_POS_LO 0x20000u
#define FX_POS_TOP (2u * FX_POS_LO - SY_BLOCK)
#define FX_POS(w) ((w) > FX_POS_TOP ? FX_POS_LO | ((w) & (FX_POS_LO - 1u)) : (w))
typedef char fx_pos_fits[SY_DELAY_FRAMES <= FX_POS_LO && 2048 <= FX_POS_LO ? 1 : -1];
#define ROUTE_G (-0.86f)                /* the part's route: -0.86, always (chorus.json: mix 0 dry -0.86); applied by
                                         * the last pass over the block before the phaser (the delay's own loop, else
                                         * sy_fx_render's) */

/* ---- the matrix on the effects (destinations 62-71 and their duplicates, the LFOs' own destinations). MEASURED
 *: the offsets of the voice started last (fxm, synth.c voice_block) add to the effect's
 * parameter in its steps (the law of every destination: source x (amount - 64) x 2), once per block, the sum
 * limited to 0..128 (128: one step past the top, which the sum reaches and a patch cannot); the same with a patch at
 * 0 (the chorus or the send then comes on). FXV: the value the effects read this block (fx_vals: the patch's
 * smoothed value pv plus fxm; without offsets pv to the bit). MEASURED: the reference spreads the change linearly over
 * the block, for a controller's change as for the matrix's. 0.91: the chorus and the delay do so too (chorus_rmp,
 * delay_rmp: their loops' times, depths, gains, feedback and LFO step from the last block's values, fxp, to this
 * block's, sample by sample; a step in a delay time was a jump in the waveform, a click, every block an LFO moved it).
 * A block in which none of theirs moved runs the loops as before (the same bits). the other users of FXV
 * (the send's dry part in mode 0 and part B's, the reverb, the colour filters' coefficients) step per block */
#define FXV(s, b) ((s)->fxv[(b) - 105])
/* the delay's shape (A 118; 6+: the triangle, measured) with the matrix's offset (destination 109, MEASURED: the
 * reference's modulated render is the twin's; ASSUMPTION: rounded to the nearest shape, a smoothed change too) */
#define DL_SHAPE(s) ((int)(FXV(s, 118) + 0.5f) > 5 ? 1 : (int)(FXV(s, 118) + 0.5f))   /* (FXV: pv to the bit
                                         * without an offset) */
#define FX_CH_ON(s) SY_FAR(fx_on)(s, 105)   /* the chorus runs */
#define FX_SEND_ON(s) SY_FAR(fx_on)(s, 113)   /* the send is open (or the drums send, synth.h dr_w: the
                                         * space runs as for B's send; mode 0 takes A's dry as before) */
static __attribute__((noinline)) int fx_on(const sy_synth *s, int b)   /* the patch's byte b or its offset above 0 */
{
    return s->patch[b] > 0 || s->fxm[b - 105] > 0.f || (b == 113 && s->dr_w > 0.f);
}
/* the reverb's pre-delay under the delay clock (B 20), MEASURED (the first arrival of an impulse, modes 2-4, every
 * room, 73-160 BPM, clock values 3-127): a clock value 1-19 (20-127 as 19) sets the pre-delay to its note value of the
 * tempo (the patch's or a MIDI clock's), A 114 then does nothing; halved until it fits the room's longest pre-delay
 * (the word's cap, sy_cln_rv.c). In modes 3 and 4 the feedback (A 115) repeats the input at that time: a clocked echo
 * with a reverb on it. The word as the reverb's time word (A 114 << 16: 256 frames a step): frames x 256; 0 none */
uint32_t hyb_rv_clk;
/* (the delay clock's note values in beats, B 20 = 0..19: delay(), dl_par, the reverb's hyb_rv_clk) */
static const float DL_BEATS[20] = {0, 0.0625f, 0.125f, 0.25f, 0.5f, 1, 2, 0.1875f, 0.375f, 0.75f, 1.5f,
    0.16667f, 0.33333f, 0.66667f, 1.33333f, 2.66667f, 3, 4, 8, 16};
/* the page B effect bytes the matrix moves (MEASURED: destinations 93-95, 102-107, 117-119 are the
 * static byte + the offset, the twin to the bit), their offsets and values at fxm / fxv [15 + j]: the boost's
 * intensity and tune, the distortion's intensity, the phaser's mix, rate, depth, frequency, feedback, spread, the
 * EQ's mid gain, frequency, Q */
const uint8_t SY_FXB[SY_FXN - 15] = {PB + 97, PB + 98, PB + 101, PB + 85, PB + 86, PB + 87, PB + 88, PB + 89, PB + 90,
                                      PB + 92, PB + 93, PB + 94};
enum { FB_BINT = 15, FB_BTUNE, FB_DIST, FB_PH };   /* (fxv's places; the phaser's six from FB_PH, the EQ's three after) */
#define FB_EQ (FB_PH + 6)
int sy_fx_slot(int b)                   /* (flash: synth.c fx_tail) byte b's place in fxm + 1, 0 if none */
{
    int j;
    if (b >= 105 && b <= 119) return b - 104;
    for (j = 0; j < SY_FXN - 15; j++) if (SY_FXB[j] == b) return 16 + j;
    return 0;
}
SY_BLKR static __attribute__((noinline)) void fx_vals(sy_synth *s)   /* (once a block: the flash, or RAM, SY_BLK_RAM) */
{
    int k, rm = 0;
    NO_UNROLL for (k = 15; k < SY_FXN; k++)   /* (page B's: pv + the offset, 0..127, a step per block) */
        s->fxv[k] = sy_clampf(s->pv[SY_FXB[k - 15]] + s->fxm[k], 0.f, 127.f);
    NO_UNROLL for (k = 0; k < 15; k++) {
        float x = s->pv[105 + k] + s->fxm[k], p;
        x = x < 0.f ? 0.f : x > 128.f ? 128.f : x;
        p = s->fx_snap ? x : s->fxv[k]; /* (the last block's value: the ramp's start) */
        s->fxp[k] = p;
        if (p != x) rm |= k < 5 ? 1 : k >= 8 && k <= 12 ? 2 : 0;   /* (chorus 105-109; send, delay 113-117) */
        s->fxv[k] = x;
    }
    s->fx_rmp = (uint8_t)rm;
    s->fx_snap = 0;
    {   /* (bpm_now: set before the effects run; the fraction of a word, 1/256 of a frame, dropped) */
        const int c = s->patch[PB + 20];
#ifdef HYB_MULTI
        if (!s->pidx)                   /* (part A's: the only space) */
#endif
        hyb_rv_clk = c ? (uint32_t)(DL_BEATS[c > 19 ? 19 : c] * (60.f * REF_FS * 256.f) / s->bpm_now) : 0u;
    }
}
/* the reverb's bytes (it reads the patch: no smoothing) with the offsets, in whole steps 0..127 (ASSUMPTION: its
 * control words take the byte; q[112..119] written) */
static __attribute__((noinline)) const uint8_t *fx_bytes(const sy_synth *s, uint8_t *q)   /* (flash) */
{
    int k;
    if (!s->fx_mod) return s->patch;
    NO_UNROLL for (k = 112; k < 120; k++) {
        const int x = (int)s->patch[k] + (k > 112 ? sy_floori(s->fxm[k - 105] + 0.5f) : 0);
        q[k] = (uint8_t)(x < 0 ? 0 : x > 127 ? 127 : x);
    }
    return q;
}

SY_COLD void sy_fx_reset(sy_synth *s)
{
    int c, i;
    for (c = 0; c < 2; c++) {
        for (i = 0; i < 1024 * 2; i++) s->ch_buf[c][i] = 0.f;
#ifdef HYB_MULTI
        if (!c && !s->pidx) for (i = 0; i < SY_DELAY_FRAMES; i++) s->dl_buf[i] = 0;   /* (part A's: the space) */
#else
        if (!c) for (i = 0; i < SY_DELAY_FRAMES; i++) s->dl_buf[i] = 0;
#endif
        for (i = 0; i < 12; i++) s->ph_ap[c][i] = 0.f;
        for (i = 0; i < 4; i++) s->eq_z[c][i][0] = s->eq_z[c][i][1] = 0.f;
        s->dl_lp[c] = s->dl_hp[c] = s->ch_fb[c] = s->ph_fb[c] = 0.f;
    }
    for (i = 0; i < 16; i++) s->rv_state[i] = 0.f;
    s->fx_snap = 1;
    for (i = 0; i < 10; i++) s->eq_key[i] = 255;
}

/* sy_cos2pi(t) for 0 <= t <= 1 (a modulator's phase): sy_sin2pi's operations, its floor of t + 0.75 (0 or 1 in this
 * range) by a compare instead of the int round trip; bit-identical */
SY_FXR static inline __attribute__((always_inline)) float cos2pi_u(float t)
{
    float x, x2;
    t += 0.25f;
    if (t + 0.5f >= 1.f) t -= 1.f;      /* (t - 0 is t: t >= 0.25 here) */
    if (t > 0.25f) t = 0.5f - t;
    else if (t < -0.25f) t = -0.5f - t;
    x = t * SY_2PI;
    x2 = x * x;
    return x * (1.f + x2 * (-0.166666667f + x2 * (0.00833333333f + x2 * (-0.000198412698f
        + x2 * (2.75573192e-6f - x2 * 2.50521084e-8f)))));
}

#ifdef HYB_FAST_FX
/* INAUDIBLE (off by default): the chorus's and the delay's sine modulator for the block in a pass of its own, by a
 * coupled-form oscillator instead of the polynomial per frame (rc -= e rs; rs += e rc: rc is cos(t + n d) after n
 * steps when rs starts at sin(t + d / 2), e = 2 sin(pi d)), re-seeded from the phase every FX_RN frames. The phase
 * runs as in the loops (the same sums); d is its actual step ((ph + dph) - ph: the sum's rounding, constant within a
 * binade), so a run follows it but across a binade or a wrap. Out of line: the loops' registers stay as they were */
#ifndef FX_RN
#define FX_RN 16
#endif
#ifdef __clang__
#define FX_NOUNROLL _Pragma("clang loop unroll(disable)")   /* (the runs of FX_RN frames: not unrolled, RAM code) */
#endif
#ifndef FX_NOUNROLL
#define FX_NOUNROLL
#endif
SY_WORK_ATTR static float fx_ub[SY_BLOCK];
/* fx_ub[i] = 0.5 - 0.5 cos(2 pi p), p the phase after i + 1 steps ph += dph (wrapped at 1), half: p + 0.5 wrapped */
SY_FXR static __attribute__((noinline)) void fx_sine_u(float ph, float dph, int half)
{
    int i, j;
    for (j = 0; j < SY_BLOCK; j += FX_RN) {
        float t = half ? (ph < 0.5f ? ph + 0.5f : ph - 0.5f) : ph, d = (ph + dph) - ph;
        float x = 3.14159265f * d, e = 2.f * x * (1.f - x * x * (1.f / 6.f));   /* (d < 0.0011: sin(pi d) exact) */
        float rc = sy_cos2pi(t), rs = sy_sin2pi(t + 0.5f * d);
        FX_NOUNROLL for (i = j; i < j + FX_RN; i++) {
            ph += dph;
            if (ph >= 1.f) ph -= 1.f;
            rc -= e * rs;
            rs += e * rc;
            fx_ub[i] = 0.5f - 0.5f * rc;
        }
    }
}
#endif

/* a modulation LFO of the chorus / delay / phaser family: unipolar 0..1, shapes 0 sine .. 5 sample & glide */
SY_FXR static float mod_u(float ph, int shape, float from, float to)
{
    switch (shape) {
    case 1: return ph < 0.5f ? 2.f * ph : 2.f - 2.f * ph;
    case 2: return ph;
    case 3: return ph < 0.5f ? 0.f : 1.f;
    case 4: return to;
    case 5: return from + ph * (to - from);
    default: return 0.5f - 0.5f * sy_cos2pi(ph);
    }
}

/* the chorus's own phases of the saw and the square (tracked against the reference: the saw starts half
 * way, the square starts with the short delay) */
SY_FXR static float mod_u_ch(float ph, int shape, float from, float to)
{
    if (shape == 2) return ph < 0.5f ? ph + 0.5f : ph - 0.5f;
    if (shape == 3) return ph < 0.5f ? 1.f : 0.f;
    return mod_u(ph, shape, from, to);
}

/* the delay's modulation phases (tracked against the reference: sine and triangle start at the short end,
 * the saw rises): sine and triangle on the phase + 0.5, the saw 0.5 - ph (wrapped), the square, sample & hold and
 * glide as mod_u's on the phase (delay_rmp's switch; delay()'s loops the first three, 1.18C26) */

SY_FXCOLD float onepole(float fs, float hz)
{
    float x = SY_2PI * hz / fs;
    return x >= 3.f ? 1.f : 1.f - sy_exp2(-x * 1.44269504f);
}

SY_FXR static inline __attribute__((always_inline)) float rd_frac(const float *buf, int mask, uint32_t w, float d)
{
    float p = (float)w - d, f;
    int i = sy_floori(p);
    f = p - (float)i;
    {
        int i0 = i & mask, i1 = (i + 1) & mask;     /* (the indices once) */
        float a = buf[i0];
        return a + f * (buf[i1] - a);
    }
}
SY_FXR static inline __attribute__((always_inline)) float rd16(const int16_t *buf, uint32_t w, float d)
{
    float p = (float)w - d, f;
    int i = sy_floori(p);
    f = p - (float)i;
    {
        int i0 = i & DL_MASK, i1 = (i + 1) & DL_MASK, a = buf[i0];
        return ((float)a + f * (float)(buf[i1] - a)) * DL_ISCALE;
    }
}
SY_FXR static inline __attribute__((always_inline)) int16_t to16(float x)
{
    x *= DL_SCALE;
    if (x > 32767.f) x = 32767.f;
    if (x < -32767.f) x = -32767.f;
    if (x > -DL_DEADBAND && x < DL_DEADBAND) return (int16_t)x;   /* (small: toward zero, see DL_DEADBAND) */
    return (int16_t)(x + (x >= 0.f ? 0.5f : -0.5f));
}

#ifndef PH_HOLD
#define PH_HOLD 1                       /* the chorus and phaser LFOs hold while the part is silent (the
                                         * delay's runs on: read on the modulation's phase after 20 and 300 blocks) */
#endif

/* ---- (0.91) the chorus's and the delay's blocks with a ramp (fx_vals' fx_rmp: one of their parameters moved since
 * the last block). MEASURED: the reference spreads a change linearly over the block. Their
 * loops as chorus()'s and delay()'s, every shape by one expression, with the times, the depth, the gains, the feedback
 * and the LFO's step at frame i of the block a + (b - a) (i + 1) / 128 (a from fxp, the last block's values; b from
 * fxv, this block's: its last frame takes this block's values, the next block starts there). In the flash: they run
 * only while a modulation or a controller moves the effect */
typedef struct { float D, d, dry, wet, fb, dph; } ch_par_t;
static void ch_par(const sy_synth *s, const float *v, ch_par_t *o)   /* chorus()'s values from v = A 105.. */
{
    float mix = v[0], fbv = v[4];
    o->D = (1.f + 8.f * v[3]) * (s->fs / REF_FS);
    o->d = v[2] / 128.f;
    o->dry = mix <= 64.f ? 1.f - 0.293f * mix / 64.f : 0.707f * (127.f - mix) / 63.f;
    o->wet = mix <= 64.f ? 0.707f * mix / 64.f : 0.707f + 0.293f * (mix - 64.f) / 63.f;
    o->fb = 0.977f * (fbv - 64.f) / 64.f;
    o->dph = sy_tab(s, T_CH_HZ, v[1]) * s->ifs;
}
static __attribute__((noinline)) void chorus_rmp(sy_synth *s, float *l, float *r)   /* (flash) */
{
    ch_par_t a, b;
    int shape = s->patch[110] > 5 ? 0 : s->patch[110], i;
    float sm = SY_FAR(onepole)(s->fs, 200.f), ph = s->ch_ph;
    float chu = s->ch_u, shf = s->ch_sh_from, sht = s->ch_sh_to;
    float *restrict bl = s->ch_buf[0], *restrict br = s->ch_buf[1];
    uint32_t w = s->ch_w;
    int hold = s->ch_hold, run = PH_HOLD == 0 || s->part_snd;
    const int mask = 2047;
    float kD, kd, kdry, kwet, kfb, kdph;
    ch_par(s, s->fxp, &a);
    ch_par(s, s->fxv, &b);
    kD = (b.D - a.D) * (1.f / SY_BLOCK); kd = (b.d - a.d) * (1.f / SY_BLOCK);
    kdry = (b.dry - a.dry) * (1.f / SY_BLOCK); kwet = (b.wet - a.wet) * (1.f / SY_BLOCK);
    kfb = (b.fb - a.fb) * (1.f / SY_BLOCK); kdph = (b.dph - a.dph) * (1.f / SY_BLOCK);
    for (i = 0; i < SY_BLOCK; i++) {
        const float t = (float)(i + 1);
        float D = a.D + kD * t, d = a.d + kd * t, fb = a.fb + kfb * t, dph = a.dph + kdph * t;
        float u, x, dl, dr, yl, yr, ph2;
        if (hold > 0) hold--;
        else if (run) ph += dph;
        if (ph >= 1.f) ph -= 1.f;
        ph2 = ph < 0.5f ? ph + 0.5f : ph - 0.5f;
        if (ph >= 0.5f && ph - dph < 0.5f) { shf = sht; sht = 0.5f + 0.5f * sy_randf(&s->ch_rng); }
        switch (shape) {                /* (chorus()'s: CH_LOOP's expressions, mod_u_ch's) */
        case 0: x = 0.5f - 0.5f * cos2pi_u(ph); break;
        case 1: x = ph < 0.5f ? 2.f * ph : 2.f - 2.f * ph; break;
        case 2: x = ph2; break;
        case 3: x = ph < 0.5f ? 1.f : 0.f; break;
        case 4: x = sht; break;
        default: x = shf + ph2 * (sht - shf); break;
        }
        chu += sm * (x - chu);
        u = chu;
        dl = sy_maxf(1.f, D * (1.f - d * u));
        dr = sy_maxf(1.f, D * (1.f - d * (1.f - u)));
        yl = rd_frac(bl, mask, w, dl);
        yr = rd_frac(br, mask, w, dr);
        bl[w & mask] = l[i] + fb * yl;
        br[w & mask] = r[i] + fb * yr;
        w++;
        {
            const float dry = a.dry + kdry * t, wet = a.wet + kwet * t;
            l[i] = dry * l[i] + wet * yl;
            r[i] = dry * r[i] + wet * yr;
        }
    }
    s->ch_u = chu; s->ch_sh_from = shf; s->ch_sh_to = sht; s->ch_hold = hold;
    s->ch_w = w;
    s->ch_ph = ph;
}

/* the chorus: D = 1 + 8 * delay frames, modulated downwards, left and right in antiphase, linear interpolation */
SY_FXR static void chorus(sy_synth *s, float *l, float *r)
{
    float mix = FXV(s, 105), rate = FXV(s, 106), depth = FXV(s, 107), dly = FXV(s, 108), fbv = FXV(s, 109);
    int shape = s->patch[110] > 5 ? 0 : s->patch[110], i;
    float sc = s->fs / REF_FS, D = (1.f + 8.f * dly) * sc;
    float dry = mix <= 64.f ? 1.f - 0.293f * mix / 64.f : 0.707f * (127.f - mix) / 63.f;
    float wet = mix <= 64.f ? 0.707f * mix / 64.f : 0.707f + 0.293f * (mix - 64.f) / 63.f;
    float fb = 0.977f * (fbv - 64.f) / 64.f, hz = sy_tab(s, T_CH_HZ, rate), ph = s->ch_ph, dph = hz * s->ifs;
    float sm = onepole(s->fs, 200.f);   /* tau 0.8 ms */
    uint32_t w = s->ch_w;
    const int mask = 2047;
    float chu = s->ch_u, shf = s->ch_sh_from, sht = s->ch_sh_to, d128 = depth / 128.f;   /* (the states in registers) */
    float *restrict bl = s->ch_buf[0], *restrict br = s->ch_buf[1];
    int hold = s->ch_hold, run = PH_HOLD == 0 || s->part_snd;
    /* the loop once per modulator shape (the shape's expression as mod_u_ch and mod_u give it, chosen outside) */
#define CH_LOOP(UEXPR) \
    for (i = 0; i < SY_BLOCK; i++) { \
        float u, dl, dr, yl, yr, ph2; \
        if (hold > 0) hold--; \
        else if (run) ph += dph;        /* holds while the part is silent, as the phaser's */ \
        if (ph >= 1.f) ph -= 1.f; \
        /* the sample & hold (and glide) draws at half the cycle (ph + 0.5 wraps) */ \
        ph2 = ph < 0.5f ? ph + 0.5f : ph - 0.5f; \
        if (ph >= 0.5f && ph - dph < 0.5f) { shf = sht; sht = 0.5f + 0.5f * sy_randf(&s->ch_rng); } \
        (void)ph2; \
        chu += sm * ((UEXPR) - chu);    /* smoothed (chorus.json: the depth falls 7 % at 50 Hz, the saw's jump is a ramp) */ \
        u = chu; \
        dl = sy_maxf(1.f, D * (1.f - d128 * u)); \
        dr = sy_maxf(1.f, D * (1.f - d128 * (1.f - u))); \
        yl = rd_frac(bl, mask, w, dl); \
        yr = rd_frac(br, mask, w, dr); \
        bl[w & mask] = l[i] + fb * yl; \
        br[w & mask] = r[i] + fb * yr; \
        w++; \
        l[i] = dry * l[i] + wet * yl; \
        r[i] = dry * r[i] + wet * yr; \
    }
    switch (shape) {                    /* (the common shapes their own loop; the rest the general expression) */
#ifdef HYB_FAST_FX
    case 0:
        if (!hold) {                    /* (a hold: the general loop, the same expression for shape 0) */
            fx_sine_u(ph, run ? dph : 0.f, 0);
            CH_LOOP(fx_ub[i])
            break;
        }
        goto ch_gen;
#else
    case 0: CH_LOOP(0.5f - 0.5f * cos2pi_u(ph)) break;
#endif
    case 1: CH_LOOP(ph < 0.5f ? 2.f * ph : 2.f - 2.f * ph) break;
    case 2: CH_LOOP(ph < 0.5f ? ph + 0.5f : ph - 0.5f) break;
    default:
#ifdef HYB_FAST_FX
    ch_gen:
#endif
        CH_LOOP(mod_u_ch(shape >= 4 ? ph2 : ph, shape, shf, sht)) break;
    }
#undef CH_LOOP
    s->ch_u = chu; s->ch_sh_from = shf; s->ch_sh_to = sht; s->ch_hold = hold;
    s->ch_w = w;
    s->ch_ph = ph;
}

/* the delay's colour filter (inside the loop): 64 flat, below a low pass, above a high pass, first order each.
 * MEASURED (a repeat over the one before, fitted within 0.3 dB): bilinear sections (the low pass
 * a zero at fs/2, the high pass unity there), the corner 1797.5 Hz x 2^(0.12138 (c - 32)) (low pass) and 269.1 Hz x
 * 2^(0.12208 (c - 96)) (high pass). The loop runs them as a state-variable one pole: v = g (x - z), y = v + z, z = y
 * + v, g = K / (1 + K), K = tan(pi fc / fs) (the high pass x - y). Returned: g (lpa, hpa: 1 / 0 when off) */
static __attribute__((noinline)) float tpt_g(const sy_synth *s, float hz)   /* (flash) a bilinear one pole's g = K / (1 +
                                         * K), K = tan(pi hz / fs), hz below 0.49 fs: the colour's, the distortion's 10 and 11 */
{
    const float k = FTANPI(sy_minf(0.49f, hz * s->ifs));
    return k / (1.f + k);
}
SY_FXCOLD void colour_coef(const sy_synth *s, float c, float *lpa, float *hpa)
{
    float k;
    *lpa = 1.f;
    *hpa = 0.f;
    if (c > 63.5f && c < 64.5f) return;
    k = SY_FAR(tpt_g)(s, c < 64.f ? 1797.5f * sy_exp2(0.121379f * (c - 32.f)) : 269.1f * sy_exp2(0.122083f * (c - 96.f)));
    if (c < 64.f) *lpa = k; else *hpa = k;
}

SY_FXCOLD void send_law(float send, float *dry, float *wet)    /* fx.json delay send: points every 16, 127 the end */
{
    static const float X[9] = {0, 16, 32, 48, 64, 80, 96, 112, 127};
    static const float DRY[9] = {1, 1, 1, 1, 1, 1, 0.97f, 0.47f, 0};
    static const float WET[9] = {0, 0.056f, 0.145f, 0.258f, 0.415f, 0.675f, 1, 1, 1};
    int i = 0;
    float f;
    while (i < 7 && send > X[i + 1]) i++;
    f = sy_clampf((send - X[i]) / (X[i + 1] - X[i]), 0.f, 1.f);
    *dry = DRY[i] + f * (DRY[i + 1] - DRY[i]);
    *wet = WET[i] + f * (WET[i + 1] - WET[i]);
}

/* the send on the input of the delay and the reverb. MEASURED: a send the matrix moves
 * acts where the signal enters them (an echo keeps the send it went in with; the dry part follows at once). The
 * output keeps the patch's own wet (pv; 1 if that is 0), the input takes the ratio. a send the patch or a
 * controller moves stays on the output as before. Without offsets 1 (x 1: the same bits) */
static __attribute__((noinline)) float send_in(const sy_synth *s, float *wet)   /* (flash) */
{
    float d0, w0, g;
    if (s->fxm[8] == 0.f) return 1.f;
    SY_FAR(send_law)(s->pv[113], &d0, &w0);
    if (w0 <= 0.f) w0 = 1.f;
    g = *wet / w0;
    *wet = w0;
    return g;
}

/* (the delay's tables: dl_par) */
static const float DL_RATIO[4] = {0.5f, 0.75f, 0.25f, 0.875f};
static const uint8_t DL_PAT[17][2] = {{1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1}, {2, 3}, {2, 5}, {3, 2}, {3, 3}, {3, 4},
    {3, 5}, {4, 3}, {4, 5}, {5, 2}, {5, 3}, {5, 4}, {5, 5}};

/* (1.18C26) the delay's loops with both taps at least DL_FAR frames back and the line's position past the longest tap
 * (every read position p >= 1, none in this block's writes): the floor by the truncation, the time without its
 * max(1, .), the read's position counted in float (exact below 2^24), the int16 line's scale 2^-13 taken into the
 * feedback and the wet (a power of two: the same products to the bit). delay(); delay_rmp's "far" loop */
#define DL_FAR ((float)(SY_BLOCK + 2))
SY_WORK_ATTR static float dl_p[2][SY_BLOCK];   /* (delay(): the block's read positions, left and right) */
SY_FXR static inline __attribute__((always_inline)) float rd16p(const int16_t *buf, float p)   /* rd16 / DL_ISCALE, p >= 0 */
{
    const int i = (int)p;
    const float f = p - (float)i;
    const int i0 = i & DL_MASK, i1 = (i + 1) & DL_MASK, a = buf[i0];
    return (float)a + f * (float)(buf[i1] - a);
}

/* (0.91) the delay's block with a ramp (fx_rmp 2: the send, time, feedback, rate or depth moved; chorus_rmp's note):
 * delay()'s values from v = A 105.., its loop with them ramped */
typedef struct { float DL, DR, dry, wet, gin, fb, hz, depth; } dl_par_t;
/* the delay's LFO rate (Hz): the chorus's table (fx.json), but 127 runs at 58.0 Hz (the chorus's and the phaser's at
 * 127: 50.05; MEASURED: the delayed sine's frequency modulation); 126-127 between */
static __attribute__((noinline)) float dl_hz(const sy_synth *s, float v)   /* (flash) */
{
    const float h = sy_tab(s, T_CH_HZ, v);
    return v > 126.f ? h + (sy_minf(v, 127.f) - 126.f) * 7.95f : h;
}
static __attribute__((noinline)) void dl_par(const sy_synth *s, const float *v, int mode, dl_par_t *o)   /* (flash: once a
                                         * block, delay() and delay_rmp) */
{
    float sc = s->fs / REF_FS, DL, DR, dry, wet, gin;
    float bpm = s->bpm_now >= 1.f ? s->bpm_now : 120.f;
    int clk = s->patch[PB + 20];
    DL = (1.f + 256.f * v[9]) * sc;
    DR = DL;
    if (mode >= 5 && mode <= 8) DR = (1.f + 256.f * v[9] * DL_RATIO[mode - 5]) * sc;
    if (clk >= 1 && mode >= 1 && mode <= 8) {
        float t = 60.f / bpm * DL_BEATS[clk > 19 ? 19 : clk] * REF_FS;
        while (t > 32768.f) t *= 0.5f;
        DL = (1.f + t) * sc;
        DR = mode >= 5 ? (1.f + t * DL_RATIO[mode - 5]) * sc : DL;
    }
    if (mode >= 9 && mode <= 25) {
        float six = 60.f / bpm / 4.f * s->fs;
        const int a = DL_PAT[mode - 9][0], b = DL_PAT[mode - 9][1];
        while (six * (float)(a > b ? a : b) > 32768.f * sc) six *= 0.5f;
        DL = sc + six * a;
        DR = sc + six * b;
    }
    if (mode == 1) DR = DL;
    o->DL = sy_clampf(DL, 1.f, (float)(SY_DELAY_FRAMES - 4));
    o->DR = sy_clampf(DR, 1.f, (float)(SY_DELAY_FRAMES - 4));
    SY_FAR(send_law)(v[8], &dry, &wet);
    gin = SY_FAR(send_in)(s, &wet);
    dry *= s->dry_g;
    if (s->mx_ovr) { dry = 0.f; wet = s->mx_w; gin = 1.f; }
    /* MEASURED (depth 0, the colour flat): the wet at its full 1 (x the route's 0.86: 0.86 of the
     * input; 0.834 before, read with the modulation's interpolation in it), the loop's gain -feedback / 128 (64: a
     * repeat at -0.500, 127: -0.992; before -0.962 fb / 127, the repeats 0.27 dB each too quiet) */
    o->dry = dry; o->wet = wet; o->gin = gin;
    o->fb = v[10] * (-1.f / 128.f);
    o->hz = dl_hz(s, v[11]) * s->ifs;
    o->depth = 0.001552f * v[12] * sy_maxf(o->DL, o->DR);
}
static __attribute__((noinline)) void delay_rmp(sy_synth *s, float *l, float *r, int mode)   /* (flash) */
{
    dl_par_t a, b;
    int i, shape = DL_SHAPE(s), one, hpon, far;
    float lpa, hpa, ph = s->dl_ph, lp0 = s->dl_lp[0], hp0 = s->dl_hp[0], shf = s->ch_sh_from, sht = s->ch_sh_to;
    float kDL, kDR, kdry, kwet, kgin, kfb, khz, kdep, lpz;
    int16_t *restrict buf = s->dl_buf;
    uint32_t w = s->dl_w;
    if (mode >= 26) mode = 1;
    dl_par(s, s->fxp, mode, &a);
    dl_par(s, s->fxv, mode, &b);
    SY_FAR(colour_coef)(s, FXV(s, 119), &lpa, &hpa);   /* (the colour: this block's, as delay()) */
    if (hpa == 0.f) hp0 = 0.f;
    hpon = hpa != 0.f;
    lpz = lpa < 1.f ? 1.f : 0.f;
    one = a.DR == a.DL && b.DR == b.DL && a.depth == 0.f && b.depth == 0.f;
    kDL = (b.DL - a.DL) * (1.f / SY_BLOCK); kDR = (b.DR - a.DR) * (1.f / SY_BLOCK);
    kdry = (b.dry - a.dry) * (1.f / SY_BLOCK); kwet = (b.wet - a.wet) * (1.f / SY_BLOCK);
    kgin = (b.gin - a.gin) * (1.f / SY_BLOCK); kfb = (b.fb - a.fb) * (1.f / SY_BLOCK);
    khz = (b.hz - a.hz) * (1.f / SY_BLOCK); kdep = (b.depth - a.depth) * (1.f / SY_BLOCK);
    /* (1.18C26) both ends' taps at least DL_FAR back and the line past the longest (delay()'s test, the ramp between
     * the two blocks' values): the far loop (rd16p; the feedback's and the wet's ramps x 2^-13, each term scaled
     * alone: the same sums to the bit). Else the near loop, as before */
    far = sy_minf(sy_minf(a.DL, b.DL), sy_minf(a.DR, b.DR)) - 1.01f * sy_maxf(a.depth, b.depth) >= DL_FAR
          && (float)w >= sy_maxf(sy_maxf(a.DL, b.DL), sy_maxf(a.DR, b.DR)) + 2.f;
#define RMP_LOOP(FAR) \
    for (i = 0; i < SY_BLOCK; i++) { \
        const float t = (float)(i + 1); \
        float u, yl, yr, xl, xr, q, dep = a.depth + kdep * t; \
        ph += a.hz + khz * t; \
        if (ph >= 1.f) { ph -= 1.f; shf = sht; sht = 0.5f + 0.5f * sy_randf(&s->ch_rng); } \
        q = ph < 0.5f ? ph + 0.5f : ph - 0.5f; \
        switch (shape) {                /* (delay()'s: DL_LOOP's expressions; 3-5 here alone, 1.18C26) */ \
        case 0: u = 0.5f - 0.5f * sy_cos2pi(q); break; \
        case 1: u = q < 0.5f ? 2.f * q : 2.f - 2.f * q; break; \
        case 2: u = ph < 0.5f ? 0.5f - ph : 1.5f - ph; break; \
        case 3: u = ph < 0.5f ? 0.f : 1.f; break; \
        case 4: u = sht; break; \
        default: u = shf + ph * (sht - shf); break; \
        } \
        if (FAR) { \
            yl = rd16p(buf, pf - (a.DL + kDL * t - dep * u)); \
            yr = one ? yl : rd16p(buf, pf - (a.DR + kDR * t - dep * (1.f - u))); \
        } else { \
            yl = rd16(buf, w, sy_maxf(1.f, a.DL + kDL * t - dep * u)); \
            yr = one ? yl : rd16(buf, w, sy_maxf(1.f, a.DR + kDR * t - dep * (1.f - u))); \
        } \
        xl = l[i]; xr = r[i]; \
        q = 0.5f * (xl + xr) * (a.gin + kgin * t) + (FAR ? fbS + kfbS * t : a.fb + kfb * t) * yl; \
        {   /* (delay()'s colour) */ \
            float v = lpa * (q - lp0); \
            q = v + lp0; lp0 = q + lpz * v; \
            if (hpon) { float y; v = hpa * (q - hp0); y = v + hp0; hp0 = y + v; q -= y; } \
        } \
        buf[w & DL_MASK] = to16(q); \
        w++; \
        pf += 1.f; \
        { \
            const float dry = a.dry + kdry * t, wet = FAR ? wetS + kwetS * t : a.wet + kwet * t; \
            l[i] = (dry * xl + wet * yl) * ROUTE_G; \
            r[i] = (dry * xr + wet * yr) * ROUTE_G; \
        } \
    }
    {
        const float fbS = a.fb * DL_ISCALE, kfbS = kfb * DL_ISCALE, wetS = a.wet * DL_ISCALE, kwetS = kwet * DL_ISCALE;
        float pf = (float)w;
        if (far) RMP_LOOP(1)
        else RMP_LOOP(0)
    }
#undef RMP_LOOP
    s->dl_lp[0] = lp0; s->dl_hp[0] = hp0; s->ch_sh_from = shf; s->ch_sh_to = sht;
    s->dl_w = w;
    s->dl_ph = ph;
}

SY_FXR static void delay(sy_synth *s, float *l, float *r, int mode)
{
    float DL, DR, dry, wet, gin, fb, lpa, hpa, hz, depth, ph = s->dl_ph;
    int i, shape = DL_SHAPE(s);
    uint32_t w = s->dl_w;
    if (mode >= 26) mode = 1;
    {   /* the times, the send, the feedback, the modulation: dl_par (the flash, once a block; delay_rmp's). One loop
         * (fx.json delay mode): the left output reads it at DL, the right taps the same line at DR (its first echo at
         * DR, then DR + k DL); the times 1 + the measured length (frames); the delay clock (B 20) and the sync
         * patterns (9-25) as dl_par has them. ASSUMPTION: the loop takes the sum of both inputs (the measurements fed
         * both alike) */
        dl_par_t P;
        SY_FAR(dl_par)(s, s->fxv, mode, &P);
        DL = P.DL; DR = P.DR; dry = P.dry; wet = P.wet; gin = P.gin; fb = P.fb; hz = P.hz; depth = P.depth;
    }
    /* (1.18C26) a tap nearer than DL_FAR (a time below ~2.8 ms; the modulation's depth taken off with a margin: its u
     * may pass 0..1 by a rounding), a line not yet past its longest tap (the first second after power-on), or the
     * square, sample & hold and glide shapes with a depth (none in the factory sounds): delay_rmp, which without a ramp
     * (fxp = fxv for the send, time, feedback, rate and depth: fx_vals) takes this block's values on every sample, the
     * same expressions as this loop had: the same bits */
    if (sy_minf(DL, DR) - 1.01f * depth < DL_FAR || (float)w < sy_maxf(DL, DR) + 2.f || (shape > 2 && depth != 0.f)) {
        SY_FAR(delay_rmp)(s, l, r, mode);
        return;
    }
    {   /* (1.18C26) the modulator and the read positions first, in a pass of their own (dl_p: the left's, the
         * right's; the one loop kept 25 values live on the target's 16 registers and spilled ~15 a sample): the
         * shape's expression as delay_rmp's switch has it, chosen outside; depth 0: the times as they are (DL - 0 u =
         * DL for any u), the phase and the sample & hold run on */
        float shf = s->ch_sh_from, sht = s->ch_sh_to, pf = (float)w;   /* (pf: the write position in float, w < 2^24
                                         * (FX_POS): + 1 a sample exact) */
        float *restrict pl = dl_p[0], *restrict pr = dl_p[1];
#define DL_POS(UEXPR) \
        for (i = 0; i < SY_BLOCK; i++) { \
            float u, q; \
            ph += hz; \
            if (ph >= 1.f) { ph -= 1.f; shf = sht; sht = 0.5f + 0.5f * sy_randf(&s->ch_rng); } \
            q = ph < 0.5f ? ph + 0.5f : ph - 0.5f; \
            (void)q; \
            u = (UEXPR); \
            pl[i] = pf - (DL - depth * u); \
            pr[i] = pf - (DR - depth * (1.f - u)); \
            pf += 1.f; \
        }
        if (depth == 0.f) DL_POS(0.f)
        else switch (shape) {           /* (0-2; the rest delay_rmp's, above) */
        case 0: DL_POS(0.5f - 0.5f * sy_cos2pi(q)) break;
        case 1: DL_POS(q < 0.5f ? 2.f * q : 2.f - 2.f * q) break;
        default: DL_POS(ph < 0.5f ? 0.5f - ph : 1.5f - ph) break;
        }
#undef DL_POS
        s->ch_sh_from = shf; s->ch_sh_to = sht;
    }
    colour_coef(s, FXV(s, 119), &lpa, &hpa);
    {   /* the taps, the loop's input, the colour, the write, the output */
        float lp0 = s->dl_lp[0], hp0 = s->dl_hp[0];   /* (the states in registers) */
        const float fbs = fb * DL_ISCALE, wets = wet * DL_ISCALE;   /* (rd16p's value is rd16's x 8192) */
        const float *restrict pl = dl_p[0], *restrict pr = dl_p[1];
        int16_t *restrict buf = s->dl_buf;
        const int one = DR == DL && depth == 0.f;   /* (no modulation: the right reads what the left read) */
        const float lpz = lpa < 1.f ? 1.f : 0.f;   /* (flat: lp0 += 1 (q - lp0), the same bits as before) */
        if (hpa == 0.f) hp0 = 0.f;      /* (no high pass: its state goes. Kept, it was subtracted from every sample as a
                                         * constant: a DC in the loop and on the output, after the colour went above 64
                                         * and back while a sound passed, through patch loads until the next power-on) */
#define DL_LOOP(HP) \
        for (i = 0; i < SY_BLOCK; i++) { \
            const float xl = l[i], xr = r[i], yl = rd16p(buf, pl[i]), yr = one ? yl : rd16p(buf, pr[i]); \
            float q = 0.5f * (xl + xr) * gin + fbs * yl; \
            { float v = lpa * (q - lp0); q = v + lp0; lp0 = q + lpz * v; }   /* (colour_coef's section; lpz 0: lp0 = q) */ \
            HP \
            buf[(w + (uint32_t)i) & DL_MASK] = to16(q); \
            l[i] = (dry * xl + wets * yl) * ROUTE_G;   /* (the part's route here: one pass less) */ \
            r[i] = (dry * xr + wets * yr) * ROUTE_G; \
        }
        if (hpa != 0.f) DL_LOOP({ float v = hpa * (q - hp0); float y = v + hp0; hp0 = y + v; q -= y; })
        else DL_LOOP(;)
#undef DL_LOOP
        s->dl_lp[0] = lp0; s->dl_hp[0] = hp0;
    }
    s->dl_w = w + SY_BLOCK;
    s->dl_ph = ph;
}

SY_FXR static void reverb(sy_synth *s, float *l, float *r, int mode)   /* (sy_cln_rv.c) */
{
    float dry, wet, gin;
    uint8_t q[120];                     /* (the patch with the matrix's offsets, when it has any: fx_bytes) */
    send_law(FXV(s, 113), &dry, &wet);
    gin = SY_FAR(send_in)(s, &wet);
    dry *= s->dry_g;                    /* B 58 */
    if (s->mx_ovr) { dry = 0.f; wet = s->mx_w; gin = 1.f; }   /* (B or the drums sending: the space's wet part alone, sy_fx_render) */
#ifdef HYB_SHIMMER
    hyb_rv_shm[0] = s->shm[0]; hyb_rv_shm[1] = s->shm[1];   /* (the matrix on FX > SHIMMER, synth.c fx_take) */
    hyb_rv_shm[2] = s->shm[2]; hyb_rv_shl3 = s->shx_rt[11] == 2;   /* (1.18C45: LFO 3 on SHIM MOD) */
#endif
    hyb_rv_run(s->rvx_ring, SY_FAR(fx_bytes)(s, q), mode, l, r, dry, wet, gin);   /* (in RAM too: RVR) */
}

/* the phaser (fx.json phaser): 1-6 second-order allpasses of a low Q (0.29: the notches of 2 sections lie 12.6:1
 * apart), the notches at 1.12 x the cutoff map of the frequency (48 -> 389 Hz), swept upwards in cutoff steps
 * (0.6 per depth step), the LFO starting at the top; dry and wet crossfade at equal power, equal at 64 (+1.5 dB).
 * ASSUMPTION: feedback and spread laws */
#ifndef PH_Q
#define PH_Q 0.287f                     /* (unused: the Q follows B 90 below) */
#endif
#ifndef PH_F0
#define PH_F0 849.f
#endif
#ifndef PH_FB
#define PH_FB 0.94f
#endif
#ifndef PH_F
#define PH_F 1.136f
#endif
SY_FXR static void phaser(sy_synth *s, float *l, float *r)
{
    int n = (s->patch[PB + 84] > 5 ? 5 : s->patch[PB + 84]) + 1, i, k, c;
    const float *P = s->fxv + FB_PH;   /* (B 85-90 with the matrix's offsets: fx_vals) */
    float v = P[0], m = v <= 64.f ? v / 128.f : 0.5f + (v - 64.f) / 126.f, hz = sy_tab(s, T_CH_HZ, P[1]);
    float dry, wet, q;
    float dep = 0.6f * P[2], fq = P[3];
    float fb = PH_FB * (P[4] - 64.f) / 64.f   /* the loop gain read from the complex response */, spread = 0.25f * P[5] / 127.f, cw[2], al[2];
    (void)m;
    if (v <= 64.f) { float q = v / 64.f; dry = 1.f - 0.302f * q; wet = 0.698f * q; }    /* the measured crossfade */
    else { float q = (127.f - v) / 63.f; wet = 1.f - 0.302f * q; dry = 0.698f * q; }
    {   /* B 90 (spread) sets the allpasses' Q, both sides alike (read from the complex response): 0 -> 2.93,
         * 32 -> 1.38, 64 -> 0.82, 96 -> 0.51, 127 -> 0.287 (log-interpolated) */
        static const float LQ[5] = {1.5510f, 0.4651f, -0.2863f, -0.9714f, -1.8009f};
        float x = P[5] * (1.f / 32.f);
        int j = x >= 3.f ? 3 : (int)x;
        float fr = sy_minf(x - (float)j, 31.f / 32.f) * (j == 3 ? 32.f / 31.f : 1.f);
        q = sy_exp2(LQ[j] + fr * (LQ[j + 1] - LQ[j]));
    }
    if (PH_HOLD == 0 || s->part_snd) {  /* the LFO holds while the part is silent: it neither runs from the load
                                         * nor restarts at a note (read on the notch's track: a note after 20, 200 or
                                         * 450 blocks starts at the same point; one after 0.8 s of silence continues
                                         * where the last left off; notes added to sounding ones do not move it) */
        s->ph_lfo += hz * s->bdt;
        s->ph_lfo -= (float)sy_floori(s->ph_lfo);
    }
    for (c = 0; c < 2; c++) {
        /* Measured: the notch at 849 Hz x 2^((fq + 1 - 64) / 13.28) (static, read from the complex response);
         * the LFO sweeps it above the knob, from the knob up by depth/128 of the way to 127.3, a triangle (tracked on
         * noise); the right side 1.105 x the left; B 90 (spread) has no effect */
        float u = sy_absf(1.f - 2.f * s->ph_lfo);  /* a triangle (the notch moves at a constant rate in octaves) */
        float st = fq + 1.f + P[2] * (1.f / 128.f) * (127.3f - fq) * u;   /* (no cap: 0.47 fs below) */
        float f = sy_minf(PH_F0 * sy_exp2((st - 64.f) * (1.f / 13.28f)) * (c ? 1.105f : 1.f), 0.47f * s->fs), w = f * s->ifs;
        (void)dep; (void)spread;
        cw[c] = sy_cos2pi(w);
        al[c] = sy_sin2pi(w) * (1.f / (2.f * q));
    }
    for (c = 0; c < 2; c++) {
        float *x = c ? r : l, *z = s->ph_ap[c], ia = 1.f / (1.f + al[c]);
        float b0 = (1.f - al[c]) * ia, b1 = -2.f * cw[c] * ia;   /* allpass: b2 = 1, a1 = b1, a2 = b0 */
        /* the stages' states in registers over the block (the same stages, the same order: transposed direct form II) */
        float z0 = z[0], z1 = z[1], z2 = z[2], z3 = z[3], z4 = z[4], z5 = z[5], z6 = z[6], z7 = z[7], z8 = z[8], z9 = z[9],
              z10 = z[10], z11 = z[11], fbv = s->ph_fb[c];
#define PH_STAGE(K, ZA, ZB) if (n > K) { float o = b0 * y + ZA; ZA = b1 * y - b1 * o + ZB; ZB = y - b0 * o; y = o; }
        for (i = 0; i < SY_BLOCK; i++) {
            float y = x[i] + fb * fbv;
            PH_STAGE(0, z0, z1)
            PH_STAGE(1, z2, z3)
            PH_STAGE(2, z4, z5)
            PH_STAGE(3, z6, z7)
            PH_STAGE(4, z8, z9)
            PH_STAGE(5, z10, z11)
            fbv = y;
            x[i] = dry * x[i] + wet * y;
        }
#undef PH_STAGE
        z[0] = z0; z[1] = z1; z[2] = z2; z[3] = z3; z[4] = z4; z[5] = z5; z[6] = z6; z[7] = z7; z[8] = z8; z[9] = z9;
        z[10] = z10; z[11] = z11; s->ph_fb[c] = fbv;
    }
    (void)k;
}

/* the EQ (fx.json eq_etc, open.json fx_more): analog-style bands, bilinear with the corner prewarped. Shelves of first
 * order, a boost 1 + (G - 1) x low pass (high pass) at a fixed corner, a cut its inverse; the mid band 1 + (G - 1) x band pass, so
 * its width grows with the gain. Recomputed when a value changes. (0.91: the coefficients' code in the flash, eq_coefs:
 * it runs when an EQ byte changes; the RAM it took, ~1.7 KB, holds the effects' ramp dispatch) */
#define EQ_COLD SY_COLD static __attribute__((noinline))
EQ_COLD float eq_x2(float x) { return FEXP2(x); }   /* (the EQ's exp2 and log2 out of line: each inlined copy costs
                                         * ~100 B of flash; the same operations) */
EQ_COLD float eq_l2(float x) { return FLOG2(x); }
EQ_COLD void bilin(float *c, float fs, float hz, const float *B, const float *A)   /* B, A: s^2, s, 1 (s / w0) */
{
    float K = 1.f / FTANPI(sy_minf(hz, 0.495f * fs) / fs), K2 = K * K;   /* (0.495: the high shelf's and the mid
                                         * band's top corners, below; 0.45 before) */
    float b0 = B[0] * K2 + B[1] * K + B[2], b1 = 2.f * (B[2] - B[0] * K2), b2 = B[0] * K2 - B[1] * K + B[2];
    float a0 = A[0] * K2 + A[1] * K + A[2], a1 = 2.f * (A[2] - A[0] * K2), a2 = A[0] * K2 - A[1] * K + A[2];
    if (B[0] == 0.f && A[0] == 0.f) {   /* first order (the shelves, the boost): a first-order section. As a biquad it had
                                         * a pole and a zero at z = -1 that cancel: that mode is never damped, and what
                                         * the rounding puts on it (a hot signal, a change of the coefficients) stayed
                                         * as a tone at fs/2 until the next power-on. The same response */
        float d0 = A[1] * K + A[2];
        c[0] = (B[1] * K + B[2]) / d0; c[1] = (B[2] - B[1] * K) / d0; c[2] = 0.f;
        c[3] = (A[2] - A[1] * K) / d0; c[4] = 0.f;
        return;
    }
    c[0] = b0 / a0; c[1] = b1 / a0; c[2] = b2 / a0; c[3] = a1 / a0; c[4] = a2 / a0;
}

EQ_COLD void band(float *c, int type, float fs, float hz, float db, float q, float e)   /* 0 low shelf, 1 peak, 2 high
                                         * shelf; e: eq_dry's share added to the dry path */
{
    float G = eq_x2(0.166096405f * db), B[3] = {0.f, 1.f, 1.f}, A[3] = {0.f, 1.f, 1.f};
    if (type == 1) { B[0] = A[0] = 1.f; B[1] = G / q; A[1] = 1.f / q; }
    else if (type == 0) { if (G >= 1.f) B[2] = G; else A[2] = 1.f / G; }   /* boost: 1 + (G - 1) LP, cut: its inverse */
    else {
        /* the high shelf, MEASURED (within 0.3 dB over the band): (G s + 1) / (s + 1), s normalized
         * and prewarped at its pole: a boost's at the corner (its zero at corner / G), a cut's at G x the corner (its
         * zero at the corner, which may lie above fs/2: B 46 = 127, 26 kHz); the pole kept below 0.4928 fs (23.1
         * kHz) by p / (1 + (p / 0.4928 fs)^16)^(1/16), applied from 0.7 of that (below it: within 0.003 dB, the
         * same coefficients as before). Before: the corner clamped at 0.45 fs, G reduced, a cut prewarped at its zero
         * (B 46 = 127, gain 10: -5.8 dB at 22.5 kHz instead of -13.4, -0.4 instead of -8.1 at 12 kHz) */
        const float cap = 0.4928f * fs;
        if (G < 1.f) hz *= G;
        if (hz > 0.7f * cap) {
            float x = hz / cap;
            x *= x; x *= x; x *= x; x *= x;
            hz /= eq_x2(eq_l2(1.f + x) * (1.f / 16.f));
        }
        B[1] = G;
    }
    B[0] += e * A[0]; B[1] += e * A[1]; B[2] += e * A[2];
    bilin(c, fs, hz, B, A);
}

typedef struct { float x, y; } pt_t;

/* the mid band's Q per B 94 (MEASURED: the reference's response fitted at every 8th value, boost and
 * cut alike within 4 %; 0.33 .. 2.73 before, 5-15 % wider below 112) */
static const pt_t EQ_Q[] = {{0, 0.281f}, {16, 0.3675f}, {32, 0.491f}, {48, 0.6565f}, {64, 0.783f}, {80, 0.9355f},
    {96, 1.316f}, {112, 1.8725f}, {127, 2.688f}};

EQ_COLD float logint(const pt_t *p, int n, float x)
{
    int i;
    float f;
    if (x <= p[0].x) return p[0].y;
    if (x >= p[n - 1].x) return p[n - 1].y;
    for (i = 1; i < n - 1 && x > p[i].x; i++)
        ;
    f = (x - p[i - 1].x) / (p[i].x - p[i - 1].x);
    return eq_x2(eq_l2(p[i - 1].y) + f * (eq_l2(p[i].y) - eq_l2(p[i - 1].y)));
}

/* the boost (B 97 intensity, B 98 tune), MEASURED (a 9 x 9 grid of the reference's responses, each
 * fitted exactly by this form): H = (A s + D) / (s + 1), bilinear at the corner 99.28 Hz x 2^(t / 24) (100 Hz .. 4
 * kHz); the high frequencies' gain A falls by i x 0.00098 x u dB, the low frequencies' D rises by
 * i x (0.07781 - 0.000615 u) dB, at most 6.02 dB (x 2), u = max(0, t - 24.46); i, t the intensity and the tune, 127
 * counting as 128. Within 0.06 dB of the fit at every grid point (before: per-tune tables, up to 2.9 dB off at
 * high intensities) */
EQ_COLD void boost_band(float *c, float fs, int iv, int tv)
{
    const float t = (float)(tv + (tv == 127)), i = (float)(iv + (iv == 127)), u = sy_maxf(0.f, t - 24.46f);
    float B[3] = {0.f, eq_x2(0.166096405f * -i * 0.00097979f * u), eq_x2(0.166096405f * sy_minf(6.016f, i * (0.077808f - 0.0006154f * u)))};
    float D[3] = {0.f, 1.f, 1.f};
    bilin(c, fs, 99.28f * eq_x2(t * (1.f / 24.005f)), B, D);
}

EQ_COLD float eq_db(int v)             /* a band's gain: MEASURED 0.2505 dB a step, 127 counting as 128 (-16.0 .. +16.05
                                         * dB; 0.254 before: 0 was -16.3) */
{
    return (float)(v + (v == 127) - 64) * 0.2505f;
}
#ifndef EQ_DRY
#define EQ_DRY 1
#endif
/* CLEANER: a band's gain 89-126 also lifts the dry path (measured, black box: bass/eqsweep2.py, shelf.py, eqmodel.py;
 * PROVENANCE 11): a parabola over each 8 values from 88, 0 at 88, 96, .., 120 and 127 (the last 7 wide), its top
 * EQ_DRY_A dB in the far band (the same for the low, mid and high band, at every frequency and Q; H + e fits within
 * 0.06 dB rms, (1 + e) H 0.09-0.14); below 89 none */
EQ_COLD float eq_dry(int v)
{
    static const float EQ_DRY_A[5] = {0.375f, 0.275f, 0.22f, 0.185f, 0.165f};
    float t;
    int k;
    if (!EQ_DRY || v <= 88 || v >= 127) return 0.f;
    k = (v - 89) >> 3;
    t = (float)(v - 88 - 8 * k) / (k == 4 ? 7.f : 8.f);
    return eq_x2(0.166096405f * EQ_DRY_A[k] * 4.f * t * (1.f - t)) - 1.f;
}
EQ_COLD void eq_coefs(sy_synth *s)     /* (flash: when a value changed) the bands from eq_key: B 92, 93, 94, 95, 96, 45,
                                         * 46, 97, 98 as they are now (eq_glide) */
{
    const uint8_t *p = s->eq_key;
    const float f = (float)p[1];
    band(s->eq_c[0], 0, s->fs, 26.3f * eq_x2((float)p[5] / 32.f), eq_db(p[3]), 0.f, eq_dry(p[3]));
    /* the mid band's centre: MEASURED 1 kHz x 2^((f - 72) / 12) (within 0.2 % from 16 to 124; a table of 17 points
     * before, within 0.6 %), above 124 to 21.8 kHz at 127 (the fit of the prewarped section) */
    band(s->eq_c[1], 1, s->fs, 1000.f * eq_x2((sy_minf(f, 124.f) - 72.f) * (1.f / 12.f) + sy_maxf(0.f, f - 124.f) * 0.03786f),
         eq_db(p[0]), logint(EQ_Q, 9, (float)p[2]), eq_dry(p[0]));
    band(s->eq_c[2], 2, s->fs, 1670.f * eq_x2((float)p[6] / 32.f), eq_db(p[4]), 0.f, eq_dry(p[4]));
    boost_band(s->eq_c[3], s->fs, p[7], p[8]);
    s->eq_on = (p[0] != 64 ? 2 : 0) | (p[3] != 64 ? 1 : 0) | (p[4] != 64 ? 4 : 0) | (p[7] ? 8 : 0);
}
/* the EQ's values this block (flash: once a block): the patch's, the mid band's and the boost's with the matrix's
 * offsets (whole steps: fxv rounded). MEASURED (the mid gain, frequency and the boost by the matrix):
 * the EQ glides to a new value at 8 steps a block (2 dB a block for a gain: +16 dB in 8 blocks); ASSUMPTION the same
 * for every EQ byte and for a knob; a patch load (eq_key 255) takes the value at once. 1: a value moved */
#if SY_BLK_RAM2
#define EQG_ATTR SY_B2
#else
#define EQG_ATTR __attribute__((noinline))
#endif
static EQG_ATTR int eq_glide(sy_synth *s)
{
    static const uint8_t K[9] = {PB + 92, PB + 93, PB + 94, PB + 95, PB + 96, PB + 45, PB + 46, PB + 97, PB + 98};
    static const int8_t V[9] = {FB_EQ, FB_EQ + 1, FB_EQ + 2, 0, 0, 0, 0, FB_BINT, FB_BTUNE};   /* (fxv's place, 0 none) */
    int k, changed = 0;
    NO_UNROLL for (k = 0; k < 9; k++) {
        const int t = V[k] && s->fx_mod ? (int)(s->fxv[V[k]] + 0.5f) : s->patch[K[k]], c = s->eq_key[k];
        if (c == t) continue;
        s->eq_key[k] = (uint8_t)(c == 255 || (c - t <= 8 && t - c <= 8) ? t : c + (t > c ? 8 : -8));
        changed = 1;
    }
    return changed;
}
SY_FXR static void eq(sy_synth *s, float *l, float *r)
{
    int i, c, k;
    if (SY_FAR(eq_glide)(s)) SY_FAR(eq_coefs)(s);
    for (k = 0; k < 4; k++) {
        static const uint8_t BIT[4] = {1, 2, 4, 8};
        const float *q = s->eq_c[k];
        if (!(s->eq_on & BIT[k])) continue;
        for (c = 0; c < 2; c++) {
            float *x = c ? r : l, z0 = s->eq_z[c][k][0], z1 = s->eq_z[c][k][1];   /* (the states in registers) */
            float q0 = q[0], q1 = q[1], q2 = q[2], q3 = q[3], q4 = q[4];
            for (i = 0; i < SY_BLOCK; i++) {    /* transposed direct form II */
                float xi = x[i], y = q0 * xi + z0;
                z0 = q1 * xi - q3 * y + z1;
                z1 = q2 * xi - q4 * y;
                x[i] = y;
            }
            s->eq_z[c][k][0] = z0; s->eq_z[c][k][1] = z1;
        }
    }
}

/* the distortion (B 100 type 1-11, B 101 intensity; addendum-1.md, dist.json): before the delay (ASSUMPTION: also
 * before the chorus). Every law below is read from dist.json; all values before the part's route (-0.857) */

/* (1.18C37) FX_DIST_FLASH: the distortion's code in the flash, its 2.4 KB of RAM code freed for code run once a block
 * (sy_int.h SY_BLK_RAM2): a type's loop runs 128 samples a block on both sides and stays in the flash's cache as the
 * other per-sample loops there do (the device's benchmark: warm loops as fast from the flash as from RAM), its few
 * set-up lines a block are what it costs; a patch without it calls nothing (the type tested here). The same code
 * (bit-identical). 0: in RAM, as 1.18C36 */
#ifndef FX_DIST_FLASH
#define FX_DIST_FLASH 1
#endif
#if FX_DIST_FLASH && defined(SY_FX_RAM)
#define SY_DISTR static __attribute__((noinline))
#define DIST_CALL(s, l, r) do { if ((unsigned)((s)->patch[PB + 100] - 1) < 11u) SY_FAR(distortion)(s, l, r); } while (0)
#define DIST_FAR(fn) SY_FAR(fn)         /* (its calls into the RAM code: far) */
#else
#define SY_DISTR SY_FXR static
#define DIST_CALL(s, l, r) distortion(s, l, r)
#define DIST_FAR(fn) fn
#endif
SY_DISTR void distortion(sy_synth *s, float *l, float *r)
{
    int t = s->patch[PB + 100], i, c;
    float it = s->fxv[FB_DIST], q = it * (1.f / 127.f);   /* (B 101 with the matrix's offset: fx_vals) */
    if (t < 1 || t > 11) return;
    for (c = 0; c < 2; c++) {
        float *x = c ? r : l;
        switch (t) {
        case 5: {                                   /* an exact hard clip, its slope and limit per intensity */
            int k = (int)sy_clampf(it + 0.5f, 0.f, 127.f);
            float g = SY_DIST5_G[k], lim = SY_DIST5_L[k];
            for (i = 0; i < SY_BLOCK; i++) x[i] = sy_clampf(g * x[i], -lim, lim);
            break;
        }
        case 1: case 2: case 3: case 4: {           /* the curves between the intensities 0, 16, .., 112, 127,
                                                     * blended linearly (sy_cln_d14.h, sy_d14_s) */
            int k = it >= 112.f ? 7 : (int)(it * (1.f / 16.f)), h;
            float w = k == 7 ? (it - 112.f) * (1.f / 15.f) : it * (1.f / 16.f) - (float)k;
            float ca[2], cb[2], ka[2], kb[2];
            const float *Sp[2];
            for (h = 0; h < 2; h++) {               /* (per half: the two nodes' scales, the blend folded in) */
                Sp[h] = CLN_D14_S[t - 1][h];
                ca[h] = (1.f - w) * CLN_D14_A[t - 1][h][k]; cb[h] = w * CLN_D14_A[t - 1][h][k + 1];
                ka[h] = CLN_D14_K[t - 1][h][k]; kb[h] = CLN_D14_K[t - 1][h][k + 1];
            }
            for (i = 0; i < SY_BLOCK; i++) {
                float v = sy_clampf(x[i], -1.f, 1.f), z = sy_absf(v);
                h = v < 0.f;
                x[i] = ca[h] * sy_d14_s(Sp[h], ka[h] * z) + cb[h] * sy_d14_s(Sp[h], kb[h] * z);
            }
            break;
        }
        case 6: {                                   /* a sine folder: D = A sin(2 pi k x), k 4 at 0 and x1.68 per 32 */
            static const float A[9] = {0.06248f, 0.06054f, 0.05858f, 0.05663f, 0.05466f,
                                       0.05278f, 0.05071f, 0.04879f, 0.04689f};   /* per 16, 127 last */
            int k = it >= 112.f ? 7 : (int)(it * (1.f / 16.f));
            float w = k == 7 ? (it - 112.f) * (1.f / 15.f) : it * (1.f / 16.f) - (float)k;
            float amp = A[k] + w * (A[k + 1] - A[k]), kk = 4.f * sy_exp2(0.748f * it * (1.f / 32.f));
            for (i = 0; i < SY_BLOCK; i++) x[i] = amp * sy_sin2pi(kk * sy_clampf(x[i], -1.f, 1.f));
            break;
        }
        case 7: {                                   /* a rectifier faded in: the input's share 1 - q^2, the rectified
                                                     * x2.0 times q^1.4 (the sine's lines per intensity), then a high
                                                     * pass at 20.9 Hz (the response at 0) */
            float dry = 1.f - q * q, rect = 2.f * sy_exp2(1.4f * sy_log2(sy_maxf(q, 1e-6f))), a = DIST_FAR(onepole)(s->fs, 20.9f);
            float z = s->ds_hp[c];
            for (i = 0; i < SY_BLOCK; i++) {
                float v = dry * x[i] + rect * sy_absf(x[i]);
                z += a * (v - z);
                x[i] = v - z;
            }
            s->ds_hp[c] = z;
            break;
        }
        case 8: {                                   /* CLEANER: a quantiser, rounded to the nearest step; it
                                                     * quantises at 0 too. The step per intensity (measured on the
                                                     * development engine as a black box, tools/measure/meas_dist89.py, a ramp in;
                                                     * fit_dist89.py): 1 / step is linear between the intensities 0,
                                                     * 16, .., 128, where the step is Q8_NODE / 512 */
            static const float Q8_INV[9] = {512.f, 256.f, 128.f, 64.f, 512.f / 12.f, 32.f, 512.f / 24.f, 16.f, 8.f};
            float q = sy_clampf(it, 0.f, 127.f) * (1.f / 16.f), w, ist, st;
            int k = (int)q;
            w = q - (float)k;
            ist = Q8_INV[k] + w * (Q8_INV[k + 1] - Q8_INV[k]);
            st = 1.f / ist;
            for (i = 0; i < SY_BLOCK; i++) x[i] = st * (float)sy_floori(x[i] * ist + 0.5f);
            break;
        }
        case 9: {                                   /* CLEANER: a rate reducer: a new sample whenever its phase
                                                     * passes 1, the output the hold of the frame before; the phase
                                                     * step per intensity (measured on the development engine as a black box,
                                                     * meas_dist89.py, fit_dist89.py: the mean hold over a ramp):
                                                     * 1 up to 1, then linear between nodes: 0.48109 at 2, then at
                                                     * 1 + k 127 / 22 the value 2^(-1.5 - k / 4) (k 1: 2^-1.5, k 8:
                                                     * 0.088725) */
            static const float R9_Q[4] = {1.f, 0.84089642f, 0.70710678f, 0.59460356f};   /* 2^(-j / 4) */
            float q = sy_clampf(it, 0.f, 127.f), inc, ph = s->ds_ph[c], h = s->ds_hold[c];
            if (q <= 1.f) inc = 1.f;
            else if (q <= 2.f) inc = 1.f - (q - 1.f) * (1.f - 0.4810875f);
            else if (q <= 1.f + 127.f / 22.f) inc = 0.4810875f + (q - 2.f) * (0.35355339f - 0.4810875f) / (127.f / 22.f - 1.f);
            else {
                float u = (q - 1.f) * (22.f / 127.f), w, va, vb;
                int k = (int)u;
                w = u - (float)k;
                va = k == 1 ? 0.35355339f : k == 8 ? 0.088725f : R9_Q[k & 3] * 0.35355339f / (float)(1 << (k >> 2));
                vb = k + 1 == 8 ? 0.088725f : R9_Q[(k + 1) & 3] * 0.35355339f / (float)(1 << ((k + 1) >> 2));
                inc = va + w * (vb - va);
            }
            inc *= REF_FS * s->ifs;
            for (i = 0; i < SY_BLOCK; i++) {
                float o = h;
                ph += inc;
                if (ph >= 1.f) { ph -= (float)(int)ph; h = x[i]; }   /* (a step above 1 below 46,875 Hz: 1 taken off
                                         * per sample) */
                x[i] = o;
            }
            s->ds_ph[c] = ph;
            s->ds_hold[c] = h;
            break;
        }
        case 10: case 11: {                         /* a one-pole low pass (10: 632 Hz at 64, 19.7 at 127) or high pass
                                                     * (11: 20.9 Hz at 0, 707 at 64, 22.7 kHz at 127), bilinear (MEASURED,
                                                     * the development notes: 11 at 127 leaves a 441 Hz sine at -58 dB, 96 2
                                                     * dB above the impulse-invariant pole it was; tpt_g, the colour's
                                                     * section) */
            const float g = SY_FAR(tpt_g)(s, t == 10 ? 632.f * sy_exp2((64.f - it) * 0.0794f) : 20.9f * sy_exp2(it * 0.0794f));
            float z = t == 10 ? s->ds_lp[c] : s->ds_hp[c];
            for (i = 0; i < SY_BLOCK; i++) {
                const float v = g * (x[i] - z), y = v + z;
                z = y + v;
                x[i] = t == 10 ? y : x[i] - y;
            }
            if (t == 10) s->ds_lp[c] = z; else s->ds_hp[c] = z;
            break;
        }
        }
    }
}

/* the part's gain over the block, from il / ir (the buffers themselves, or the sum: the copy folded in); out of
 * line: inlined, the pointers spill around the effects */
SY_FXCOLD void gain_ramp(float *l, float *r, const float *il, const float *ir, float g0, float dg)
{
    int i;
    if (dg == 0.f) { if (g0 != 1.f) for (i = 0; i < SY_BLOCK; i++) { l[i] = il[i] * g0; r[i] = ir[i] * g0; } }   /* (g0 + 0 (i + 1)
                                         * is g0; x 1 is x: copied already) */
    else
    for (i = 0; i < SY_BLOCK; i++) { float g = g0 + dg * (float)(i + 1); l[i] = il[i] * g; r[i] = ir[i] * g; }
}

#ifdef HYB_MULTI
SY_MX_ATTR
#else
SY_WORK_ATTR
#endif
static float mx_t[2][SY_BLOCK];         /* (part A's signal while the space takes B's or the drums' send too) */
SY_FXR void sy_fx_render(sy_synth *s, float *l, float *r, const float *ml, const float *mr)
{
    int i, mode = s->patch[112], ch = FX_CH_ON(s), dl = mode && FX_SEND_ON(s), routed = 0;
    float mx_d = 0.f;
#ifdef HYB_MULTI
    /* two parts: part B has no delay or reverb of its own (its mode byte not read: nothing of A's cleared), its
     * send goes into part A's (s->mx_bl, mx_wb, sy_multi_render); A's runs while either sends */
    if (s->pidx) { mode = s->fx_mode_last; dl = 0; }
    else if (s->mx_on && mode) dl = 1;
#endif
    SY_FAR(fx_vals)(s);                 /* (this block's values of the parameters the matrix may move) */
    /* the sum into the part's buffers, unless the gain ramp below reads it from the sum itself (no distortion, a
     * gain to apply): the same test again there (nothing kept live across the distortion: the target has 16
     * registers) */
#define FX_COPY_DUE (s->patch[PB + 100] - 1u < 11u || (dg == 0.f && g0 == 1.f))
    {
        float g0 = s->part_g_prev, dg = (s->part_g - g0) * (1.f / SY_BLOCK);
        if (FX_COPY_DUE) for (i = 0; i < SY_BLOCK; i++) { l[i] = ml[i]; r[i] = mr[i]; }
    }
    s->ch_w = FX_POS(s->ch_w);          /* (the positions in one binade, FX_POS_LO) */
    s->dl_w = FX_POS(s->dl_w);
    if (mode != s->fx_mode_last) {      /* the delay and the reverb share the memory */
        int c, k;
        for (k = 0; k < SY_DELAY_FRAMES; k++) s->dl_buf[k] = 0;
        (void)c;
        for (k = 0; k < 16; k++) s->rv_state[k] = 0.f;
        SY_FAR(hyb_rv_reset)();         /* (its ring is this memory) */
        s->fx_mode_last = (uint8_t)mode;
    }
    DIST_CALL(s, l, r);                 /* (no type: no call into the flash) */
    {   /* the patch and part volume after the distortion, ramped over the block */
        float g0 = s->part_g_prev, dg = (s->part_g - g0) * (1.f / SY_BLOCK);
        gain_ramp(l, r, FX_COPY_DUE ? l : ml, FX_COPY_DUE ? r : mr, g0, dg);
        s->part_g_prev = s->part_g;
    }
#undef FX_COPY_DUE
    if (ch) {                           /* (0.91: a block in which one of its parameters moved: the ramp, chorus_rmp) */
        if (s->fx_rmp & 1) SY_FAR(chorus_rmp)(s, l, r);
        else chorus(s, l, r);
    }
#ifdef HYB_MULTI
    if (s->pidx) {                      /* part B: its signal to A's space (by its send's wet), its dry by the send's law */
        float dry = s->dry_g, wet = 0.f, sv = s->send < 0 ? FXV(s, 113) : (float)s->send;
        if (sv > 0.f) { send_law(sv, &dry, &wet); dry *= s->dry_g; }
        s->mx_wb = wet;
        if (wet > 0.f) for (i = 0; i < SY_BLOCK; i++) { s->mx_bl[i] = l[i]; s->mx_br[i] = r[i]; }
        for (i = 0; i < SY_BLOCK; i++) { l[i] = l[i] * dry * ROUTE_G; r[i] = r[i] * dry * ROUTE_G; }
        routed = 1;
    } else
#endif
    if (dl) {
        const float dw = s->dr_w;
#ifdef HYB_MULTI
        if (s->mx_on || dw > 0.f)
#else
        if (dw > 0.f)
#endif
        {                               /* B or the drums send: the space takes A's signal and theirs, each scaled by
                                         * its send's wet over the largest (W, the space's output gain); A's dry part
                                         * kept apart and added after (the space's own dry 0: mx_ovr). The drums'
                                         * signal comes with each instrument's wet in it (the app): 1 / W more */
            float wa, wb = 0.f, W, ka, kd, k2 = 0.f;
            const float *pl = l, *pr = r;   /* (the second signal into the loop: B's, else the drums', else none) */
            send_law(FXV(s, 113), &mx_d, &wa);
            mx_d *= s->dry_g;
#ifdef HYB_MULTI
            if (s->mx_on) wb = s->mx_wb;
#endif
            W = wa > wb ? wa : wb;
            if (dw > W) W = dw;
            ka = wa / W; kd = 1.f / W;
#ifdef HYB_MULTI
            if (s->mx_on) { pl = s->mx_bl; pr = s->mx_br; k2 = wb / W; } else
#endif
            if (s->dr_on) { pl = s->dr_b; pr = pl + SY_BLOCK; k2 = kd; }   /* (none this block: zeros) */
            for (i = 0; i < SY_BLOCK; i++) {
                mx_t[0][i] = l[i]; mx_t[1][i] = r[i];
                l[i] = l[i] * ka + pl[i] * k2;
                r[i] = r[i] * ka + pr[i] * k2;
            }
#ifdef HYB_MULTI
            if (s->mx_on && s->dr_on) { /* (both B and the drums) */
                pl = s->dr_b;
                for (i = 0; i < SY_BLOCK; i++) { l[i] += pl[i] * kd; r[i] += pl[SY_BLOCK + i] * kd; }
            }
#endif
            s->mx_ovr = 1; s->mx_w = W;
        }
        if (mode >= 2 && mode <= 4) reverb(s, l, r, mode);
        else {                          /* (the delay's loop applies ROUTE_G itself; 0.91: delay_rmp as the chorus's) */
            if (s->fx_rmp & 2) SY_FAR(delay_rmp)(s, l, r, mode);
            else delay(s, l, r, mode);
            routed = 1;
        }
        if (s->mx_ovr) {
            const float k = routed ? mx_d * ROUTE_G : mx_d;
            s->mx_ovr = 0;
            for (i = 0; i < SY_BLOCK; i++) { l[i] += k * mx_t[0][i]; r[i] += k * mx_t[1][i]; }
        }
    } else {
        float dry = s->dry_g;           /* B 58, no send */
        if (!mode && FX_SEND_ON(s)) {   /* mode 0: the send still takes the dry away (fx.json delay mode 0) */
            float wet;
            send_law(FXV(s, 113), &dry, &wet);
            dry *= s->dry_g;            /* B 58 */
        }
        /* the dry gain and the route in one pass (two multiplies, as before: not one product) */
        for (i = 0; i < SY_BLOCK; i++) { l[i] = l[i] * dry * ROUTE_G; r[i] = r[i] * dry * ROUTE_G; }
        routed = 1;
    }
    if (!routed) for (i = 0; i < SY_BLOCK; i++) { l[i] *= ROUTE_G; r[i] *= ROUTE_G; }   /* (after the reverb) */
    if (s->patch[PB + 85] || s->fxm[FB_PH] > 0.f) phaser(s, l, r);   /* (its mix or its offset) */
    eq(s, l, r);
#if RVF_GATE
    if (hyb_rv_arm && dl && mode >= 2 && mode <= 4) SY_FAR(hyb_rv_post)(l, r);   /* (the float ring's rest, sy_cln_rv.c
                                         * RVF_GATE: the part's output peak, after every gain but the master volume) */
#endif
}

#if SY_FX_REST
/* ---- the effects at rest (synth.c sy_render, render2 for part A): while the chain takes blocks of exact zeros (no
 * voice, no input route), no patch byte, controller glide or effect mode changes, and the part is silent (the
 * chorus's and the phaser's LFOs hold), a block that left the chain's positions as they were (the chorus's LFO and
 * sample & hold, the phaser's LFO, the rate reducer's: bit for bit) and its decaying states (filters, allpasses,
 * feedback) as they were or below the smallest normal float (2^-126: a subnormal, a limit cycle of the float's own
 * rounding), with an output below that too, FXR_HOLD times in a row, and buffers below it (the chorus's if it runs;
 * the delay's 16-bit loop all 0, or the reverb's network, if it runs), rests: those subnormals are set to 0 (a
 * difference below 2^-126; an output sample or a sum of the next note absorbs it whole once it is above ~2^-102, so
 * the 24-bit output stays the same to the bit) and from there the chain is a fixed point: blocks of zeros give zeros
 * and change nothing but what moves by itself. Those blocks are skipped (sy_fx_rest_step: zeros out; the write
 * positions, the delay's LFO with its sample & hold draws, the reverb's pointers, phase and pre-delay glide advanced
 * as the chain would advance them; the gain ramp's start). A tail above 2^-126 (a delay's limit cycle in its 16-bit
 * loop) keeps the chain running as before. SY_FX_REST_TINY 0: exact zeros only (a subnormal limit cycle, which most
 * patches with the EQ or the phaser on end in after a note, then keeps the chain running); SY_FX_REST 0: never */
#ifndef SY_FX_REST_TINY
#define SY_FX_REST_TINY 1
#endif
/* (1.18C39) SY_FX_REST_LOW: "tiny" is below 2^-30 (-180 dB, part B's FXQ_T), not only a subnormal.
 * A state of the EQ, the boost or the phaser at the bass (a pole near 1, tens of dB of gain there) fell ~46 dB a second
 * from -100 dBFS to 2^-126: 13 s with the whole chain running after the reverb's rest (RVF_GATE, the tailtest case
 * eqmax). Set to 0 at the rest, the
 * states below 2^-30 change the output by less than 2^-30 x their gain, under the 24-bit output's last bit (2^-23);
 * the chain's output and the chorus's buffer are below 2^-30 too, the delay's loop and the reverb's ring 0 as before.
 * 0: a zero or a subnormal only (1.18C38) */
#ifndef SY_FX_REST_LOW
#define SY_FX_REST_LOW 1
#endif
#if SY_FX_REST_LOW && SY_FX_REST_TINY
#define FXR_TINY(u) (((u) & 0x7f800000u) < 0x30800000u)   /* (|x| < 2^-30: the exponent's bits below 97) */
#elif SY_FX_REST_TINY
#define FXR_TINY(u) (((u) & 0x7f800000u) == 0u)   /* (a zero or a subnormal: the exponent's bits 0) */
#else
#define FXR_TINY(u) (((u) << 1) == 0u)            /* (+0 or -0) */
#endif
#define FXR_HOLD 4                      /* still blocks before the buffers are looked at */
#define FXR_BACK 256                    /* after a look that found a sample: blocks before the next */
#define FXR_NP 10                       /* the positions (bit for bit; the last two ints) */
#define FXR_ND 54                       /* the decaying states */
SY_WORK_ATTR static uint32_t fxr_snap[2][FXR_NP + FXR_ND];   /* (per part) the states before a checked block */
#ifdef HYB_MULTI
#define FXR_P(s) ((s)->pidx)
#else
#define FXR_P(s) 0
#endif
typedef union { float f; uint32_t u; } fxr_w;
static void fxr_route(const sy_synth *s, int *mode, int *dl)   /* sy_fx_render's choice of the delay / reverb */
{
    int m = s->patch[112], d = m && FX_SEND_ON(s);
#ifdef HYB_MULTI
    if (s->pidx) { m = s->fx_mode_last; d = 0; }
    else if (s->mx_on && m) d = 1;
#endif
    *mode = m;
    *dl = d;
}
static float *fxr_dec(sy_synth *s, int k)   /* the decaying state k */
{
    if (k < 2) return &s->ch_fb[k];
    if (k < 4) return &s->dl_lp[k - 2];
    if (k < 6) return &s->dl_hp[k - 4];
    if (k < 8) return &s->ds_hp[k - 6];
    if (k < 10) return &s->ds_lp[k - 8];
    if (k < 12) return &s->ds_hold[k - 10];
    if (k < 14) return &s->ph_fb[k - 12];
    if (k < 38) return &s->ph_ap[(k - 14) / 12][(k - 14) % 12];
    return &s->eq_z[(k - 38) / 8][(k - 38) / 2 % 4][(k - 38) % 2];
}
static void fxr_states(sy_synth *s, uint32_t *p)
{
    fxr_w w;
    int k;
    w.f = s->ch_ph; p[0] = w.u; w.f = s->ch_u; p[1] = w.u; w.f = s->ch_sh_from; p[2] = w.u; w.f = s->ch_sh_to; p[3] = w.u;
    w.f = s->ph_lfo; p[4] = w.u; w.f = s->ds_ph[0]; p[5] = w.u; w.f = s->ds_ph[1]; p[6] = w.u; w.f = s->ds_cnt; p[7] = w.u;
    p[8] = (uint32_t)s->ch_hold; p[9] = s->ch_rng;
    for (k = 0; k < FXR_ND; k++) { w.f = *fxr_dec(s, k); p[FXR_NP + k] = w.u; }
}
static int fxr_same(sy_synth *s, const uint32_t *p0)   /* the positions as they were; each decaying state as it was,
                                         * or it and what it was both tiny */
{
    uint32_t p[FXR_NP + FXR_ND];
    int k;
    fxr_states(s, p);
    for (k = 0; k < FXR_NP; k++) if (p[k] != p0[k]) return 0;
    for (k = FXR_NP; k < FXR_NP + FXR_ND; k++)
        if (p[k] != p0[k] && !(FXR_TINY(p[k]) && FXR_TINY(p0[k]))) return 0;
    return 1;
}
static int fxr_tiny(const float *x, int n, int clr)   /* every x tiny (clr: then set to 0) */
{
    const uint32_t *u = (const uint32_t *)(const void *)x;
    uint32_t o = 0u;
    int i;
    for (i = 0; i < n; i++) {
        if (!FXR_TINY(u[i])) return 0;
        o |= u[i];
    }
    if (clr && (o << 1) != 0u) for (i = 0; i < n; i++) ((float *)x)[i] = 0.f;   /* (only if one is not a zero) */
    return 1;
}
static int fxr_bufs(sy_synth *s)        /* the buffers the chain reads: tiny (then 0) */
{
    int mode, dl, i;
    fxr_route(s, &mode, &dl);
    if (FX_CH_ON(s) && !(fxr_tiny(s->ch_buf[0], 2048, 0) && fxr_tiny(s->ch_buf[1], 2048, 0))) return 0;
    if (dl) {
        if (mode >= 2 && mode <= 4) {
            if (!SY_FAR(hyb_rv_still)(s->rvx_ring, SY_FX_REST_TINY)) return 0;   /* (and set to 0) */
        } else {
            const uint32_t *d = (const uint32_t *)(const void *)s->dl_buf;   /* (two frames a word: all 0) */
            uint32_t o = 0u;
            for (i = 0; i < SY_DELAY_FRAMES / 2; i += 4) o |= d[i] | d[i + 1] | d[i + 2] | d[i + 3];
            if (o) return 0;
        }
    }
    if (FX_CH_ON(s)) { fxr_tiny(s->ch_buf[0], 2048, 1); fxr_tiny(s->ch_buf[1], 2048, 1); }
    for (i = 0; i < FXR_ND; i++) {      /* the decaying states: the tiny ones 0 */
        float *f = fxr_dec(s, i);
        fxr_tiny(f, 1, 1);
    }
    return 1;
}
/* the block's start (before the chain's job is handed out): zin 1 if the chain's input is all zeros. 1: the block is
 * at rest (sy_fx_rest_step instead of sy_fx_render); 0: sy_fx_render, then sy_fx_rest_end */
SY_COLD int sy_fx_rest_begin(sy_synth *s, int zin)
{
    int wake = s->fx_wake;
    s->fx_wake = 0;
    s->fx_rc = 0;
    if (!zin || wake || s->sm_dirty || s->part_snd || s->patch[112] != s->fx_mode_last || !PH_HOLD
#ifdef HYB_MULTI
        || (s->fx_rest && !s->pidx && (s->mx_on != s->fx_rmx || s->mx_wb != s->fx_rwb))
#endif
        ) {
        s->fx_rest = s->fx_rz = 0;
        s->fx_rn = 0;
        return 0;
    }
    if (s->fx_rest) return 1;
    s->fx_rc = 1;
    if (s->fx_rz) fxr_states(s, fxr_snap[FXR_P(s)]);   /* (the last block's output was tiny: this one may count) */
    return 0;
}
/* after sy_fx_render's block (l, r its output): counts the blocks that changed nothing, looks at the buffers */
SY_COLD void sy_fx_rest_end(sy_synth *s, const float *l, const float *r)
{
    int was = s->fx_rz;
    if (!s->fx_rc) return;
    s->fx_rc = 0;
    s->fx_rz = (uint8_t)(fxr_tiny(l, SY_BLOCK, 0) && fxr_tiny(r, SY_BLOCK, 0));
    if (!s->fx_rz || !was || !fxr_same(s, fxr_snap[FXR_P(s)])) {
        if (s->fx_rn > 0) s->fx_rn = 0;
        return;
    }
    if (s->fx_rn < 0) { s->fx_rn++; return; }   /* (waiting after a look) */
    if (++s->fx_rn < FXR_HOLD) return;
    if (fxr_bufs(s)) {
        s->fx_rest = 1;
#ifdef HYB_MULTI
        s->fx_rmx = s->mx_on;
        s->fx_rwb = s->mx_wb;
#endif
    } else s->fx_rn = -FXR_BACK;
}
/* a block at rest: what sy_fx_render would leave (zeros out; the positions that move by themselves) */
void sy_fx_rest_step(sy_synth *s, float *l, float *r)
{
    int i, mode, dl;
#ifdef FXR_DEBUG
    { extern unsigned fxr_dbg; fxr_dbg++; }   /* (a host test's count of the blocks at rest) */
#endif
    fxr_route(s, &mode, &dl);
    SY_FAR(fx_vals)(s);
    for (i = 0; i < SY_BLOCK; i++) l[i] = r[i] = 0.f;
    s->ch_w = FX_POS(s->ch_w);
    s->dl_w = FX_POS(s->dl_w);
    s->part_g_prev = s->part_g;
    if (FX_CH_ON(s)) s->ch_w += SY_BLOCK;   /* (the chorus's LFO holds: the part is silent) */
    if (!dl) return;
    if (mode >= 2 && mode <= 4) {
        { uint8_t q[120]; SY_FAR(hyb_rv_quiet)(s->rvx_ring, SY_FAR(fx_bytes)(s, q), mode); }   /* (its pointers, phase word, pre-delay glide) */
        return;
    }
    {   /* the delay's LFO (it runs on), its sample & hold draws, as delay() */
        float ph = s->dl_ph, hz = dl_hz(s, FXV(s, 116)) * s->ifs, shf = s->ch_sh_from, sht = s->ch_sh_to;
        for (i = 0; i < SY_BLOCK; i++) {
            ph += hz;
            if (ph >= 1.f) { ph -= 1.f; shf = sht; sht = 0.5f + 0.5f * sy_randf(&s->ch_rng); }
        }
        s->ch_sh_from = shf; s->ch_sh_to = sht;
        s->dl_ph = ph;
        s->dl_w += SY_BLOCK;
    }
}
#endif

#ifdef HYB_MULTI
/* ---- part B at rest (synth.c render2): while part B has no voice and no input, its effects
 * take blocks of zeros. Once their output and every state they decay are below FXQ_T (2^-30, -180 dB) and the states
 * that move without a signal (the chorus's and phaser's LFOs, which hold while the part is silent; the rate reducer's
 * phase) stood still over a block, the next blocks of zeros would change nothing but the chorus's write position
 * (advanced as the chorus would) and the tails below FXQ_T: render2 skips them (sy_fx_quiet) until a voice, an input
 * route or a patch change comes. Not in the single engine. */
#ifndef FXQ_T
#define FXQ_T 9.31322575e-10f
#endif
#define FXQ_NP 10
#define FXQ_ND 50
SY_MX_ATTR static uint32_t fxq_p[FXQ_NP];   /* the positional states before the block (bits) */
SY_MX_ATTR static float fxq_d[FXQ_ND];      /* the decaying states before the block */
typedef union { float f; uint32_t u; int32_t i; } fxq_w;
static void fxq_pos(const sy_synth *s, uint32_t *p)
{
    fxq_w w;
    w.f = s->ch_ph; p[0] = w.u; w.f = s->ch_u; p[1] = w.u; w.f = s->ch_sh_from; p[2] = w.u; w.f = s->ch_sh_to; p[3] = w.u;
    w.f = s->ph_lfo; p[4] = w.u; w.f = s->ds_ph[0]; p[5] = w.u; w.f = s->ds_ph[1]; p[6] = w.u; w.f = s->ds_cnt; p[7] = w.u;
    p[8] = (uint32_t)s->ch_hold; p[9] = s->ch_rng;
}
static const float *fxq_dec(const sy_synth *s, int k)   /* the decaying state k (filters, allpasses, holds) */
{
    if (k < 2) return &s->ds_hp[k];
    if (k < 4) return &s->ds_lp[k - 2];
    if (k < 6) return &s->ds_hold[k - 4];
    if (k < 30) return &s->ph_ap[(k - 6) / 12][(k - 6) % 12];
    if (k < 32) return &s->ph_fb[k - 30];
    if (k < 48) return &s->eq_z[(k - 32) / 8][(k - 32) / 2 % 4][(k - 32) % 2];
    return &s->ch_fb[k - 48];
}
void sy_fx_snap(const sy_synth *s)      /* before the effects' block: what sy_fx_still compares with */
{
    int k;
    fxq_pos(s, fxq_p);
    for (k = 0; k < FXQ_ND; k++) fxq_d[k] = *fxq_dec(s, k);
}
static int fxq_small(const float *x, int n)
{
    int i;
    for (i = 0; i < n; i++) if (!(x[i] < FXQ_T && x[i] > -FXQ_T)) return 0;
    return 1;
}
/* after it (the block's output ol / or_, the send bl / br if it sends): 1 = at rest */
int sy_fx_still(const sy_synth *s, const float *ol, const float *or_, const float *bl, const float *br)
{
    uint32_t p[FXQ_NP];
    int k;
    fxq_pos(s, p);
    for (k = 0; k < FXQ_NP; k++) if (p[k] != fxq_p[k]) return 0;
    for (k = 0; k < FXQ_ND; k++) {      /* (below FXQ_T, or not moved: a state of an effect that is off) */
        fxq_w a, b;
        a.f = *fxq_dec(s, k); b.f = fxq_d[k];
        if (a.u != b.u && !fxq_small(&a.f, 1)) return 0;
    }
    if (!fxq_small(ol, SY_BLOCK) || !fxq_small(or_, SY_BLOCK)) return 0;
    if (s->mx_wb > 0.f && (!fxq_small(bl, SY_BLOCK) || !fxq_small(br, SY_BLOCK))) return 0;
    return 1;
}
int sy_fx_buf_still(const sy_synth *s)  /* the chorus's buffer below FXQ_T (or the chorus off: not read, not written),
                                         * and the distortion's output 0 for 0 (the measured curves 1-4 may give a
                                         * constant: the effects after it would then settle on a level of their own) */
{
    int t = s->patch[PB + 100];
    (void)t;                            /* (CLEANER: the curves 1-4 give 0 for 0: sy_d14_s) */
    return !FX_CH_ON(s) || (fxq_small(s->ch_buf[0], 2048) && fxq_small(s->ch_buf[1], 2048));
}
/* B's send at rest held at zeros (mx_bz) once B's path to the space keeps nothing above 2^-126 (the chorus's buffer,
 * the distortion's filter and hold states: zero or subnormal), so that its run on zeros could give the space nothing
 * but subnormals, which a sum with any sample of A above ~2^-102 absorbs whole: the space then gets what it got, to
 * the bit in practice (its 16-bit delay rounds the same). Looked at every FXQ_SUBN blocks while B rests and sends */
#define FXQ_SUB(u) (((u) & 0x7f800000u) == 0u)
#define FXQ_SUBN 32
static int fxq_sub(const float *x, int n)
{
    const uint32_t *u = (const uint32_t *)(const void *)x;
    int i;
    for (i = 0; i < n; i++) if (!FXQ_SUB(u[i])) return 0;
    return 1;
}
void sy_fx_quiet(sy_synth *s, int sp, float *l, float *r)   /* a block skipped: what sy_fx_render would leave
                                         * besides the effects' states (the gain ramp's start, the send's wet for part
                                         * A). sp: part A's space runs, so it takes B's send, if B sends: then B's path
                                         * to it runs as it would (from zeros: the distortion's curves 1-5 and 8 give
                                         * 0 for 0 and keep nothing; the others run), into l, r (scratch), so the space
                                         * gets the very same signal (it rounds to 16 bits: a different tail far below
                                         * hearing would round differently there), until that path holds subnormals
                                         * at most: then zeros (mx_bz, above; A's space may rest then too, sy_fx_rest_*);
                                         * the phaser, the EQ and B's output stay skipped */
{
    static uint8_t look;
    float dry = s->dry_g, wet = 0.f, sv;
    int i, t = s->patch[PB + 100];
    SY_FAR(fx_vals)(s);
    sv = s->send < 0 ? FXV(s, 113) : (float)s->send;
    if (sv > 0.f) SY_FAR(send_law)(sv, &dry, &wet);
    s->mx_wb = wet;
    if (sp && wet > 0.f && !s->mx_bz && !(++look & (FXQ_SUBN - 1))
        && fxq_sub(s->ds_hp, 2) && fxq_sub(s->ds_lp, 2) && fxq_sub(s->ds_hold, 2)
        && (!FX_CH_ON(s) || (fxq_sub(s->ch_buf[0], 2048) && fxq_sub(s->ch_buf[1], 2048)))) {
        for (i = 0; i < SY_BLOCK; i++) s->mx_bl[i] = s->mx_br[i] = 0.f;
        s->mx_bz = 1;
    }
    if (sp && wet > 0.f && !s->mx_bz) {
        float g0 = s->part_g_prev;
        for (i = 0; i < SY_BLOCK; i++) l[i] = r[i] = 0.f;
        if (t == 6 || t == 7 || t >= 9) {
            SY_FAR(distortion)(s, l, r);
            SY_FAR(gain_ramp)(l, r, l, r, g0, (s->part_g - g0) * (1.f / SY_BLOCK));
        }
        if (FX_CH_ON(s)) {
            if (s->fx_rmp & 1) SY_FAR(chorus_rmp)(s, l, r);   /* (as sy_fx_render) */
            else SY_FAR(chorus)(s, l, r);
        }
        for (i = 0; i < SY_BLOCK; i++) { s->mx_bl[i] = l[i]; s->mx_br[i] = r[i]; }
    } else if (FX_CH_ON(s)) s->ch_w = FX_POS(s->ch_w + SY_BLOCK);   /* (the chorus's write position as it would be: its reads take
                                         * the position in float, their fraction's rounding depends on it) */
    s->part_g_prev = s->part_g;
}
#endif
