/* SPDX-License-Identifier: GPL-3.0-only */
/* Internal: what the control side (synth.c, once per block) hands the audio side (sy_voice.c, per sample). */
#ifndef SY_INT_H
#define SY_INT_H
#include "synth.h"
#include "sy_math.h"
#ifdef SY_CTL_RAM
#define SY_INL inline __attribute__((always_inline))   /* (into the control blocks' RAM code: no call to the flash) */
#else
#define SY_INL inline
#endif
#include "sy_tables.h"

#ifndef SY_WORK_ATTR                    /* the block buffers' place (a firmware may give its pool: SY_WORK_POOL) */
#ifdef SY_WORK_POOL
#define SY_WORK_ATTR __attribute__((section(".pool")))
#else
#define SY_WORK_ATTR
#endif
#endif

#ifndef SY_MX_ATTR                      /* two parts (HYB_MULTI): their block buffers' place (default: .bss, beside the
                                         * pool the single engine fills) */
#define SY_MX_ATTR
#endif

#define PB 128                          /* page B */
#define REF_FS 46875.f                  /* the reference's rate: frame counts in the spec are at this rate */

typedef struct {
    uint32_t inc;                       /* phase step (2^32 = one cycle) */
    float dtw;                          /* polyBLEP width as a share of a cycle */
    float idtw;                         /* 1 / dtw (blep's, once a block) */
    uint32_t blo, bspan;                /* the phases where blep() is surely 0: ph - blo < bspan (osc_setup) */
    float ws, wsaw, c;                  /* sine/wave weight, saw weight, second saw's weight */
    uint32_t d;                         /* second saw's offset (pulse width) */
    int wave;                           /* 0 sine, 1 triangle, 2-63 table, 64 narrow pulse */
    const float *wt;                    /* the table level for this pitch */
    float raw;                          /* 1/G: the output with a unit fundamental (FM, ring) */
    float ring;                         /* the ring modulator's factor on this oscillator */
    float tkn, tkm, tkf;                /* (tick clock) ticks per cycle (0: not on ticks), the pulse edge's whole
                                         * ticks past the half and its fraction (the value on the edge's tick) */
} sy_osc_t;
/* blep() (sy_voice.c) is 0 from t = 8 dt to t = 1 - 1.5 dt (its own bounds, as it computes them): those phases,
 * less a margin of 2048 (t is the phase rounded to a float: 512 at most here), as ph - *lo < *span; none when the
 * window is too narrow (span 0) */
static inline __attribute__((always_inline)) void sy_blep_window(float dt, uint32_t *lo, uint32_t *span)
{
    float a = 8.f * dt * 4294967296.f, b = (1.f - 1.5f * dt) * 4294967296.f;
    *lo = 0u; *span = 0u;
    if (a < 4.0e9f && b > 1.0e8f && b - a > 8192.f) {
        *lo = (uint32_t)a + 2048u;
        *span = (uint32_t)b - 2048u - *lo;
    }
}

typedef struct {
    sy_osc_t o1, o2, o3;
    int o3_on;
    float g1, g2, g3;                   /* osc 1, 2 (balance x osc volume), osc 3 levels */
    float sub_g, sub_tri, sub_dt;       /* sub: level, triangle (1) or square (0), polyBLEP width */
    uint32_t sub_inc;
    float ring_g, noise_g, n_lpa, n_lpb, n_hpa, n_gain, nf_gain;
    float ring_s, sub_gb;               /* the sub's part in the ring's first factor and its balance gain with the ring
                                         * on (RING_SUB, synth.c); ring_s 0: none */
    int n_mode;                         /* 0 white, 1 low pass, 2 high pass */
    float olp_a, olp_b;                 /* the oscillators' top roll-off: a bilinear one-pole */
    float fm_i;                         /* modulation index */
    int fm_mode, sync, fm_saw;
    uint32_t sync_p0;
#ifndef SUB_C
#define SUB_C 0xC0000000u                /* the sub against osc 1's phase / 2 */
#endif
#ifndef SUB_SQ_C
#define SUB_SQ_C 0x40000000u
#endif                   /* osc 2's phase after a sync reset */
    float sat_w;                        /* saturation type 7: the rectifier's share (the drive) */
#ifdef HYB_MULTI
    const float *sat_lut;               /* (two parts: the voice's part's curve of types 1-4) */
#endif
    float sat_a7, sat_b7, sat_g5;               /* type 7 dry and rectified gains, measured */
    int use_input;                      /* input mode 1: the input instead of the oscillators */
    float in_g;
    /* filters */
    int m1, m2, route;
    float m4_g;                         /* filter 1 mode 4: the static path's level */
    float f1x;                          /* filter 1 mode 4 / 6: the coloured noise's level there (F1_STALE), 0 none */
    float g1e, h1e, k1, pg1;            /* filter 1 at the block's end */
    float g2e, h2e, k2, pg2;
    float clk_g, clk_rate;              /* the start transient's size and speed */
    float lad_ss1;                      /* stage 2's own (the self-oscillation's limiter at high resonance, mode 7) */
    float lad_ss;                       /* the analog stages' saturation scale (sy_voice.c LAD_STS) */
    float lad_a, lad_k, lad_gin, lad_gout, lad_c, bp1, bp2, lad_nz;  /* analog modes: the stages' coefficient, the feedback, input and output gain */
    float bal_a, bal_b, bal_c;          /* routing gains (see sy_voice.c) */
    /* saturation */
    int sat;
    float sat_a, sat_b, sat_c;          /* type parameters (hold samples, step, coefficient; height; hold) */
    float shaper_k;
    /* amp */
    float gle, gre;                     /* amp x pan at the block's end */
    int knee;                           /* the attack's end inside the block (frame), SY_BLOCK = none */
    int expo;                           /* interpolate the gain exponentially (decay, release) */
    float gkl, gkr;                     /* the gains there */
    int atk;                            /* the amp attack runs at the block's start: its curve sample by sample */
    int dip;                            /* high keys: the dip filter on the oscillators (b0 b1 b2 a1 a2) */
    float dp[5];
    float an0, al0, alen;               /* (see sy_env_t) */
    float afl, afr;                     /* amp x pan without the envelope */
    int tk;                             /* (HYB_OSC_TICK) osc 1 and 2 run on the tick clock (sy_voice.c) */
    float tk_r;                         /* the tick clock's step per pass (two passes per sample) */
    uint32_t tk_inc1, tk_inc2;          /* the phase steps per tick: osc 1 (on its grid), osc 2 */
    float tk_ea, tk_eb;                 /* the emphasis: tk_eb x[n] - tk_ea x[n-1] */
    float co_q;                         /* (CLN_OSC) the steps' share off the clock: s->osc_clean */
    int co_v6;                          /* (CLN_OSC) the 6-voice state (synth.c CO_V6): half the ticks, their own responses */
    int pl_v6;                          /* the 6-voice state on the plain oscillators (sync, FM: sy_voice.c pl6) */
} sy_vblk_t;

/* the amp attack sample by sample (read through the voice with a constant input): the ramp's first frame is
 * 0, its value is taken every 8 frames from the attack's start and interpolated linearly in between (ramps of 5-6
 * frames: the grid one frame earlier); ramps shorter than 5 frames run per frame, centred 0.67 frames before the
 * attack's first frame */
SY_MATHINL float sy_atk_curve(float n, float l0, float len)
{
    float c0, c1, k;
    if (len < 5.f) {
        c0 = l0 + (n + 3.33f) / len - 0.5f;
        return c0 < l0 ? l0 : c0 < 1.f ? c0 : 1.f;
    }
    if (len < 6.f) n += 1.f;
    k = (float)(int)(n * 0.125f) * 8.f;
    c0 = l0 + k / len; c1 = l0 + (k + 8.f) / len;
    if (c0 > 1.f) c0 = 1.f;
    if (c1 > 1.f) c1 = 1.f;
    return c0 + (c1 - c0) * (n - k) * 0.125f;
}
static SY_INL float sy_atk_end(float l0, float len)   /* the frame where the curve reaches full level */
{
    float n = (1.f - l0) * len;
    if (len < 5.f) { n += 0.5f * len - 3.33f; return n > 0.f ? (float)(int)(n + 0.999f) : 0.f; }
    n = (float)(int)(n * 0.125f + 0.999f) * 8.f;
    return len < 6.f ? n - 1.f : n;
}

#ifndef FS_PG
#define FS_PG 1                         /* CLEANER (FILT KNOBS SMOOTH): 1: while a base value glides, the filters'
                                         * passband gain (by the resonance) moves across the block (sy_voice.c vr_filt;
                                         * synth.c flt_glide sets sy_voice_t pr) instead of stepping at its start: else
                                         * the resonance's glide steps a little at every block (-46 dB re a sine
                                         * against -72 dB) */
#endif
void sy_voice_render(sy_synth *s, sy_voice_t *v, const sy_vblk_t *b, float *out_l, float *out_r, const float *in);
#ifdef SY_PROF
extern uint32_t (*sy_prof_clock)(void);
extern uint32_t sy_prof_v[3];           /* the first core's voices' stages this block: oscillators, filters, amp (ticks) */
#endif
#ifdef SY_CTL_RAM
#define SY_COLD __attribute__((minsize))   /* (1.00, the firmware: the flash's code run per event or once, not per
                                         * sample, small: the same arithmetic, fewer bytes) */
#else
#define SY_COLD
#endif
#ifdef __clang__
#define NO_UNROLL _Pragma("clang loop unroll(disable)")   /* (a loop kept a loop: the flash's few bytes, 0.86) */
#else
#define NO_UNROLL
#endif
/* SY_BLK_RAM (1.18C36, CPU plan section 15, stage A): code run once a block (the effects' or a voice's block set-up,
 * the voices' split between the cores) from the flash into the RAM code, in the room the reverb's swap slot (sy_cln_rv.c
 * RV_SLOT) freed: on the FM-1 such code finds its flash lines cold every block (~1 tick of 24 MHz a byte run, the
 * device's benchmark). The two-part Bluetooth firmware only (HYB_RT_SPARE, the slot's build); the same functions, the
 * same instructions (noinline as they were: out of line already), the calls from the flash through SY_FAR. 0: in the
 * flash, as 1.18C35 */
#if !defined(SY_BLK_RAM) && defined(HYB_RT_SPARE) && defined(SY_CTL_RAM) && defined(SY_HOT_RAM) && !defined(PLAT_HOST)
#define SY_BLK_RAM 1
#endif
#ifndef SY_BLK_RAM
#define SY_BLK_RAM 0
#endif
#ifndef SY_BLK_RAM_NS
#define SY_BLK_RAM_NS SY_BLK_RAM         /* (a note start's co_sdm too; 0: it stays in the flash, the measurement build's room
                                         * for its marks) */
#endif
#if SY_BLK_RAM_NS
#define SY_BLKR_NS __attribute__((section(".ram_text"), noinline))
#else
#define SY_BLKR_NS
#endif
#if SY_BLK_RAM
#define SY_BLKR __attribute__((section(".ram_text"), noinline))
#define SY_BLKC(fn) SY_FAR(fn)          /* (a call from the flash to an SY_BLKR function) */
#else
#define SY_BLKR
#define SY_BLKC(fn) fn
#endif
/* SY_BLK_RAM2 (1.18C37): more code run once a block (a voice's or the part's per-block helpers, the reverb's path, the
 * EQ's glide, the detune's wanders) into the RAM code, in the room the distortion left (sy_fx.c FX_DIST_FLASH); each
 * with the attributes it had (SY_B2M: minsize as SY_COLD / SY_LATE gave it; SY_B2: as it was), only its section
 * changed: the same instructions. Every call into them is through SY_FAR already (their callers in RAM called the
 * flash). 0: in the flash as 1.18C36 */
#ifndef SY_BLK_RAM2
#define SY_BLK_RAM2 SY_BLK_RAM
#endif
#if SY_BLK_RAM2
#define SY_B2M __attribute__((section(".ram_text"), noinline, minsize))
#define SY_B2 __attribute__((section(".ram_text"), noinline))
#endif
/* a call from the flash into RAM code (SY_HOT_RAM): too far for a direct call, so through a pointer */
#ifdef SY_HOT_RAM
static inline __attribute__((always_inline)) void *sy_far(void *p) { void *volatile q = p; return q; }
#define SY_FAR(fn) ((__typeof__(&fn))sy_far((void *)&fn))
#else
#define SY_FAR(fn) fn
#endif
/* the same with core's block buffers (sy_work.vw[core]): a voice rendered on the second core (synth.c) */
void sy_voice_render_w(sy_synth *s, sy_voice_t *v, const sy_vblk_t *b, float *out_l, float *out_r, const float *in, int core);
void sy_sat_build(sy_synth *s, int type, float drive);
void sy_waves_build(sy_wtab_t *w, int id);
void sy_fx_render(sy_synth *s, float *l, float *r, const float *ml, const float *mr);
void sy_fx_reset(sy_synth *s);
#ifndef SY_FX_REST
#define SY_FX_REST 1                    /* the effects at rest skipped, exactly (sy_fx.c); 0: they always run */
#endif
#if SY_FX_REST
int sy_fx_rest_begin(sy_synth *s, int zin);
void sy_fx_rest_end(sy_synth *s, const float *l, const float *r);
void sy_fx_rest_step(sy_synth *s, float *l, float *r);
#endif
#ifdef HYB_MULTI
void sy_fx_snap(const sy_synth *s);     /* part B at rest (sy_fx.c, synth.c render2) */
int sy_fx_still(const sy_synth *s, const float *ol, const float *or_, const float *bl, const float *br);
int sy_fx_buf_still(const sy_synth *s);
void sy_fx_quiet(sy_synth *s, int sp, float *l, float *r);
#endif

#ifdef HYB_FAST_VOICE                   /* INAUDIBLE, off by default (sy_voice.c): the voices' sine from a table */
#define SY_SIN_N 2048
extern const float SY_SIN_TAB[SY_SIN_N + 1];   /* sin(2 pi i / SY_SIN_N), i = 0..SY_SIN_N (sy_tables.c) */
#endif

/* the saturation curve of types 1-4 for the current drive (sy_voice.c builds it, the voices read it) */
#define SAT_N 512
#define SAT_RANGE 2.f
/* types 1-6 were fitted through the static path at -4.6 dB; the path is -3.8 dB (filt.json open), the curves keep
 * their measured output */
#define SAT_TRIM 0.905f
extern float sy_sat_lut[SAT_N + 1];

/* the part's distortion (B 100 / 101): the curves of types 1-5, over u in [-1, 1] with x = u |u| (sy_dist_tab.c) */
#define BP0 1.4225f                     /* the band pass: unity at resonance 0 (1 / its Q), Q times that above */
extern const uint8_t SY_FXB[SY_FXN - 15];   /* (sy_fx.c) the page B effect bytes the matrix moves, fxm / fxv [15..] */
int sy_fx_slot(int b);                  /* (sy_fx.c) byte b's place in fxm + 1, 0 if none */
extern const float SY_DIST5_G[128], SY_DIST5_L[128];
#include "sy_cln_d14s.h"               /* (CLEANER: distortion 1-4, measured: sy_d14_s; the tables in sy_dist_tab.c) */

/* the arpeggiator's patterns 1-61 (sy_arp_tab.c) */
#define SY_ARP_MAX 42
extern const uint8_t SY_ARP_N[61];
extern const uint8_t SY_ARP_EV[61][SY_ARP_MAX][2];

#ifdef SY_CTL_RAM
__attribute__((always_inline))
#endif
static SY_INL float sy_cut_hz(const sy_synth *s, float steps)
{
    float x = steps + 64.f, l, hi;
    int i;
    if (x < 0.f) x = 0.f;
    if (x > 255.99f) x = 255.99f;
    i = (int)x;
    l = s->cmap[i] + (x - (float)i) * (s->cmap[i + 1] - s->cmap[i]);
    hi = sy_minf(23000.f, 0.489f * s->fs); /* (the reference reaches 22.9 kHz: filt2.json peaks, filt.json open) */
    return sy_clampf(sy_exp2(l), 24.7f, hi);   /* the static low pass reads 24.75 Hz at cutoff 0 */
}

#endif
