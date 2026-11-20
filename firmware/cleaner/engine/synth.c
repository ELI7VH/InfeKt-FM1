/* SPDX-License-Identifier: GPL-3.0-only */
/* The control side: the patch, MIDI, the voices and their allocation, envelopes, LFOs, the matrix and every
 * per-block value of a voice (control values per 128 frames, the pitch held per block, gains
 * and the cutoff interpolated per sample). The audio side is sy_voice.c, the part's effects sy_fx.c. */
#ifdef PTAB_DEBUG
#include <stdio.h>
#endif
#ifdef HYB_TUNE
#include <stdlib.h>
#endif
#include "sy_int.h"
#if defined(SY_FX_PIPE) && SY_CORES > 1
static int (*cpu2_run)(void (*fn)(uint32_t), uint32_t arg);
#define SY_PIPE_ON(s) (((s)->opt & (SY_OPT_LATENCY | SY_OPT_FXPIPE)) == (SY_OPT_LATENCY | SY_OPT_FXPIPE) && cpu2_run)
#else
#define SY_PIPE_ON(s) 0
#endif
#ifdef SY_CTL_RAM
#define SY_CTLI __attribute__((always_inline)) inline   /* (small helpers of the control blocks, inlined into their
                                         * RAM code: no call to the flash) */
#else
#define SY_CTLI
#endif
#ifdef SY_CTL_RAM
#define SY_LATE __attribute__((section(".sy_late_const"), noinline, minsize))   /* (code of the flash placed after all the other
                                         * code, as the FM-1 app's UI_LATE: the RAM reverb's jump into the flash has
                                         * little reach to spare) */
#else
#define SY_LATE
#endif
/* (1.18C37) SY_NSL: a note start's code of the flash (alloc, unison, voice_start and the first block's set-up) as
 * SY_LATE, grouped in one section (SY_NS_GROUP: the firmware's link gives its bounds, _sy_ns_start .. _sy_ns_end, and
 * the app's main loop reads it through now and then so that its lines are in the flash's cache when a chord comes:
 * fm1/app.c NS_WARM). The same code; without SY_NS_GROUP SY_LATE itself */
/* (1.18C37) the per-block helpers SY_BLK_RAM2 takes into RAM (sy_int.h), else as they were */
#if SY_BLK_RAM2
#define B2_COLD SY_B2M
#define B2_LATE SY_B2M
#define B2_FEXP2(x) SY_FAR(sy_exp2_f)(x)   /* (the flash's, far: the same function) */
#define B2_FLOG2(x) SY_FAR(sy_log2_f)(x)
#define B2C(fn) SY_FAR(fn)              /* (a call from the flash into one of them) */
#else
#define B2C(fn) fn
#define B2_FEXP2(x) FEXP2(x)
#define B2_FLOG2(x) FLOG2(x)
#define B2_COLD SY_COLD __attribute__((noinline))
#define B2_LATE SY_LATE
#endif
#if defined(SY_CTL_RAM) && defined(SY_NS_GROUP)
#define SY_NSL __attribute__((section(".sy_ns_const"), noinline, minsize))
#else
#define SY_NSL SY_LATE
#endif

#ifndef CLK_K
#define CLK_K (-0.215f)             /* (the sign: the voice path inverts; the size against a sine at 27.5 Hz) */
#endif
#include "sy_hyb_lfow.h"
#define CLN_FILT_K_HERE
#define CLN_FILT_QC_HERE
#include "sy_cln_filt.h"             /* (CLEANER: filter 1's frequency and damping per cutoff value, measured) */
#include "sy_cln_sat5.h"
#include "sy_cln_osc.h"            /* (CLEANER: the own oscillator's clock and levels, measured) */
#include "sy_cln_fm.h"               /* (CLEANER: the FM index per amount, measured) */             /* (CLEANER: saturation 5 per drive, measured) */

/* ---- the patch bytes used by name */
enum {
    PORTA = 5, PART_VOL = 7, PAN = 10,
    O1_SHAPE = 17, O1_PW, O1_WAVE, O1_SEMI, O1_KF, O2_SHAPE, O2_PW, O2_WAVE, O2_SEMI, O2_DET, O2_FM, O2_SYNC,
    O2_FENV_PITCH, O2_FENV_FM, O2_KF,
    OSC_BAL = 33, SUB_VOL, SUB_SHAPE, OSC_VOL, NOISE_VOL, RING_VOL, NOISE_COL,
    CUT1 = 40, CUT2, RESO1, RESO2, FENV1, FENV2, FKF1, FKF2, FBAL, SAT_TYPE, A50, F1_MODE, F2_MODE, ROUTE,
    FE_A = 54, FE_D, FE_S, FE_ST, FE_R, AE_A, AE_D, AE_S, AE_ST, AE_R,
    L1_RATE = 67, L1_SHAPE, L1_ENV, L1_MODE, L1_SYM, L1_KF, L1_TRIG, L1_O1, L1_O2, L1_PW, L1_RESO, L1_FGAIN,
    L2_RATE = 79, L2_SHAPE, L2_ENV, L2_MODE, L2_SYM, L2_KF, L2_TRIG, L2_SHAPEAMT, L2_FM, L2_CUT1, L2_CUT2, L2_PAN,
    PATCH_VOL = 91, TRANSPOSE = 93, KEY_MODE = 94, UNISON = 97, UNI_DET, UNI_SPREAD, UNI_PHASE, IN_MODE, IN_SEL,
    B_ARP = PB + 1, B_ARP_PAT, B_ARP_OCT, B_ARP_HOLD, B_ARP_LEN, B_ARP_SWING,
    B_L3_RATE = PB + 7, B_L3_SHAPE, B_L3_MODE, B_L3_KF, B_L3_DEST, B_L3_AMT, B_L3_FADE,
    B_TEMPO = PB + 16, B_CLK_ARP, B_CLK_L1, B_CLK_L2, B_CLK_DL, B_CLK_L3,
    B_SMOOTH = PB + 25, B_BEND_UP, B_BEND_DN,
    B_FPOL1 = PB + 30, B_FPOL2, B_LINK, B_FBASE, B_FM_MODE, B_INIT_PH, B_PUNCH,
    B_O3_MODE = PB + 41, B_O3_VOL, B_O3_SEMI, B_O3_DET,
    B_V_O1SH = PB + 47, B_V_O2SH, B_V_PW, B_V_FM, B_V_FE1 = PB + 54, B_V_FE2, B_V_RES1, B_V_RES2,
    B_OUT2 = PB + 58, B_V_AMP = PB + 60, B_V_PAN,
    B_MX = PB + 64, B_L1_DEST = PB + 79, B_L1_AMT, B_L2_DEST, B_L2_AMT
};

/* matrix destinations -> the patch byte they move; 0 = none */
static const uint8_t DEST[120] = {
    0, PATCH_VOL, PATCH_VOL, PAN, TRANSPOSE, PORTA, O1_SHAPE, O1_PW, O1_WAVE, O1_SEMI, O1_KF,          /* 0-10 */
    O2_SHAPE, O2_PW, O2_WAVE, O2_SEMI, O2_DET, O2_FM, O2_FENV_PITCH, O2_FENV_FM, O2_KF, OSC_BAL,       /* 11-20 */
    SUB_VOL, OSC_VOL, NOISE_VOL, CUT1, CUT2, RESO1, RESO2, FENV1, FENV2, FKF1, FKF2, FBAL, /* 21-32 */
    FE_A, FE_D, FE_S, FE_ST, FE_R, AE_A, AE_D, AE_S, AE_ST, AE_R,                                      /* 33-42 */
    L1_RATE, L1_SYM, L1_O1, L1_O2, L1_PW, L1_RESO, L1_FGAIN, L2_RATE, L2_SYM,                         /* 43-51 */
    L2_SHAPEAMT, L2_FM, L2_CUT1, L2_CUT2, L2_PAN, 0 /* 57: LFO 3 rate, below */, 0, 0, 0, 0,         /* 52-61 */
    105, 106, 107, 108, 109, 113, 114, 115, 116, 117,                                                   /* 62-71 */
    B_V_O1SH, B_V_O2SH, B_V_PW, B_V_FM, B_V_FE1, B_V_FE2, B_V_RES1, B_V_RES2, B_V_AMP, B_V_PAN,      /* 72-81 */
    0, 0, 0, 0, 0, 0, B_INIT_PH, 0, RING_VOL, NOISE_COL, 119,                                         /* 82-92 */
    PB + 97, PB + 98, PB + 101, PB + 99, B_O3_VOL, B_O3_SEMI, B_O3_DET, 0, 0,                         /* 93-101 */
    PB + 85, PB + 86, PB + 87, PB + 88, PB + 89, PB + 90,                                             /* 102-107 */
    116, 118, 119, 114, 115, B_OUT2, 0, 0, 0, PB + 92, PB + 93, PB + 94};                              /* 108-119 */
/* (93-95: the boost's intensity and tune, the distortion's intensity; 102-107 the phaser's mix, rate, depth,
 * frequency, feedback, spread; 117-119 the EQ's mid gain, frequency, Q: MEASURED, each the static
 * byte + the offset in the reference (the twin), to the bit; before: none. The part's, as the effects': sy_fx.c
 * SY_FXB. MEASURED (72-127): 72-81 the velocity amounts B 47-50, 54-57, 60, 61 (until
 * 1.00 72 osc 1's shape, 78 / 79 resonance 1, 81 the pan: the reference's twin is the velocity byte's, to the bit),
 * 88 the osc init phase B 35 (ASSUMPTION: read at the note's start only), 91 the noise colour, 97-99 osc 3's volume,
 * semitone, detune, per voice; 96 the input ring modulator B 99 and 113 the dry level B 58, the part's (the voice
 * started last, as the effects'); 109 the delay's shape. 82-87, 89, 100, 101, 114-116, 120-127: none in the
 * reference (two base sounds)) */
/* matrix CC sources 3-18 */
static const uint8_t SRC_CC[16] = {1, 2, 3, 4, 6, 8, 9, 11, 12, 13, 14, 15, 16, 64, 65, 66};
/* clock note values 1-19 in quarter notes */
static const float CLK_Q[20] = {0, 0.0625f, 0.125f, 0.25f, 0.5f, 1, 2, 0.1875f, 0.375f, 0.75f, 1.5f,
    0.16666667f, 0.33333333f, 0.66666667f, 1.33333333f, 2.66666667f, 3, 4, 8, 16};

/* the free-running LFOs' phases at the start (open.json lfo23, mod.json lfo1_keytrig: the reference's free-running
 * LFOs start at fixed phases) */
#ifndef LFO1_PH0                        /* CLEANER: the part's LFO 1 and 2 at the start (measured on the development engine as a
                                         * black box: lfo_null.py, the null of a first note -90 / -121 dB) */
#define LFO1_PH0 0.52736f
#endif
#ifndef LFO2_PH0
#define LFO2_PH0 0.52736f
#endif
#ifndef LFO3_PH0
#define LFO3_PH0 0.86f
#endif

#ifndef IPH_RESET
#define IPH_RESET 1                     /* CLEANER: the osc init phase resets the handed-over phases (synth.c note_voice) */
#endif
#ifndef PH2_LOCK
#define PH2_LOCK 1                      /* CLEANER: a fresh voice's osc 2 locked to its osc 1 (synth.c ptab_take) */
#endif
#ifndef PH2_D
#define PH2_D 0.461f                    /* (osc 2 - osc 1 read 0.455 cycle on the reference's fresh voices; 0.006 the
                                         * two paths' own difference here) */
#endif
#ifndef PH_STEP
#define PH_STEP 0.38f
#endif
#ifndef UNI_LFO
#define UNI_LFO 1                       /* CLEANER: the unison voices' LFO phases (synth.c uni_lfo_ph; PROVENANCE 11) */
#endif
#ifndef UNI_LFO_T
#define UNI_LFO_T 0.091f
#endif
#ifndef PW_PH
#define PW_PH 1                         /* CLEANER: a pulse width's change moves osc 1's and 2's phases (synth.c; PROVENANCE 11) */
#endif
#ifndef PW_PH_K
#define PW_PH_K 0.38f
#endif
/* CLEANER: the oscillators' phases at a note-on (the phase hand-over). This engine's own code, built to the behaviour
 * measured on the development engine as a black box (tools/measure/ph_meas.py, ph_null.py; PROVENANCE.md "phase hand-over"):
 *  - a voice released keeps its three phases (as they are at the release) in a table of PT_N entries, each with a
 *    stamp: a counter that rises by 2 per voice started and falls by 1 per release; the release takes the entry with
 *    the lowest stamp;
 *  - a voice started takes the entry with the highest stamp (> 0) and clears it; with none, each oscillator a phase
 *    of the part's own random generator (uniform and independent, as measured: ph_meas.py "fresh");
 *  - after the start the table holds one entry, the phases a first note takes (CLN_PH_FIRST, measured: ph_null.py);
 *  - a held voice stolen keeps its phases too, once the note's voices are started (STEAL_KEEP).
 * The order (which entry a voice takes) is checked bit for bit against the development engine on note sequences that draw no
 * random phase (ph_null.py). */
#define PHASE_TAB 1
#define PT_N 16
#define CLN_LFO_TAB 1                   /* the free LFOs' start phase of a voice added to sounding ones (sy_render) */
/* (the start phases by the count of voice starts since the load, in 1 / 65536 cycle: lfo_meas.py, the development engine's
 * voices' LFO 2 on the pan against this engine's at phase 0; nearly 0.375 cycle a step) */
static const uint16_t CLN_LFO_PH[16] = {21905u, 42312u, 1480u, 25983u, 50577u, 9653u, 34138u, 58842u, 17809u, 46445u, 5557u,
    30061u, 54709u, 13731u, 38234u, 62938u};
#ifndef SUB_C                           /* the sub's start phase against half osc 1's: three quarters of a cycle
                                         * (ph_null.py: the null against the development engine's first note -150 dB) */
#define SUB_C 0xc0000000u
#endif
#ifndef CLN_PH_FIRST1                   /* (the first note's phases, osc 1-3: ph_null.py, a search on the null against
                                         * the development engine's first note: -134 dB) */
#if PH2_LOCK
#define CLN_PH_FIRST1 0xe9470512u              /* CLEANER: osc 1 of the first note 0.364 cycle earlier than the development engine's
                                         * (measured, black box: the reference's first note against this engine's,
                                         * sines at keys 48, 60, 72: +0.362 .. +0.367; bass/first.py, PROVENANCE 11) */
#else
#define CLN_PH_FIRST1 0x46761fb1u
#endif
#define CLN_PH_FIRST2 0x460f7403u
#define CLN_PH_FIRST3 0xa60806feu
#endif
static uint32_t prng_draw(sy_synth *s)   /* xorshift32 (Marsaglia), the part's own seed */
{
    uint32_t x = s->prng[0];
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return s->prng[0] = x;
}
static void ptab_init(sy_synth *s)
{
    s->prng[0] = 0x6d2b79f5u;
    s->pcnt = 1;
    s->ptab[0].st = 1;
    s->prng[1] = 0;                     /* (the fresh voices' phase step count: PH2_LOCK) */
    s->ptab[0].p1 = CLN_PH_FIRST1; s->ptab[0].p2 = CLN_PH_FIRST2; s->ptab[0].p3 = CLN_PH_FIRST3;
#if PH2_LOCK
    s->ptab[0].p2 = CLN_PH_FIRST1 + (uint32_t)(int32_t)(PH2_D * 4294967296.f);   /* (the first note's osc 2 as a fresh
                                         * voice's: 0.455 cycle from osc 1 on the reference, not in phase) */
#endif
}
static void ptab_take(sy_synth *s, sy_voice_t *v)
{
    int k, best = -1;
    int32_t b = 0;
    s->pcnt += 2;
    for (k = 0; k < PT_N; k++) if (s->ptab[k].st > b) { b = s->ptab[k].st; best = k; }
    if (best >= 0) {
        v->ph1 = s->ptab[best].p1; v->ph2 = s->ptab[best].p2; v->ph3 = s->ptab[best].p3;
        s->ptab[best].st = 0;
    } else {
        v->ph1 = prng_draw(s); v->ph2 = prng_draw(s); v->ph3 = prng_draw(s);
#if PH2_LOCK
        /* CLEANER: a fresh voice's osc 1 and 2 (measured, black box: the fresh voices of an 8-key chord and a unison
         * note; bass/fresh.py, orel.py, PROVENANCE 11): each fresh voice PH_STEP cycle on from the one before, from
         * the first note's (osc 2 against osc 1 an octave up / down: -0.38 a voice, two octaves down -0.14: both
         * oscillators step alike), osc 2 PH2_D from its osc 1 in every voice (+-0.003). Osc 3 a random draw (not
         * measured) */
        s->prng[1] += (uint32_t)(PH_STEP * 4294967296.f);
        v->ph1 = CLN_PH_FIRST1 + s->prng[1];
        v->ph2 = v->ph1 + (uint32_t)(int32_t)(PH2_D * 4294967296.f);
#endif
    }
    v->used = 1;
}
static void ptab_put(sy_synth *s, const uint32_t *h, uint32_t p1, uint32_t p2, uint32_t p3)
{
    int k, best = 0;
    for (k = 1; k < PT_N; k++) if (s->ptab[k].st < s->ptab[best].st) best = k;
    s->pcnt -= 1;
    s->ptab[best].st = s->pcnt;
    if (s->keep_prev) { s->ptab[best].p1 = h[0]; s->ptab[best].p2 = h[1]; s->ptab[best].p3 = h[2]; }   /* (the arp's
                                         * release: the phases of an earlier block, keep_prev) */
    else { s->ptab[best].p1 = p1; s->ptab[best].p2 = p2; s->ptab[best].p3 = p3; }
}
static void ptab_keep(sy_synth *s, const sy_voice_t *v) { ptab_put(s, v->hph[s->arp_mark & 3], v->ph1, v->ph2, v->ph3); }
static void ptab_keep_st(sy_synth *s, const sy_steal_t *v) { ptab_put(s, v->hph[s->arp_mark & 3], v->ph1, v->ph2, v->ph3); }
SY_CTLI static float fm_law(const float *t, float x)   /* straight between nodes 4 steps apart (x in 0..127) */
{
    int i = (int)(x * 0.25f);
    return t[i] + (x - 4.f * (float)i) * 0.25f * (t[i + 1] - t[i]);
}
/* a transcendental's last argument and value at one call site of one voice (c[0], c[1]; the key MEMO_NONE before the
 * first call): the same argument gives the same bits, so the value is computed only when the argument changed */
#define MEMO_NONE 1e30f
#ifdef SY_CTL_RAM
/* (S1: the control blocks' RAM code evaluates 2^x at ~35 sites, each an inlined copy of the polynomial (SY_MATHINL:
 * always_inline): one RAM function instead, the same arithmetic, a call (~10 instructions) per evaluation; the
 * memos below call it only when their argument changed) */
static __attribute__((section(".ram_text"), noinline)) float ctl_exp2(float x) { return sy_exp2(x); }
static __attribute__((section(".ram_text"), noinline)) float ctl_log2(float x) { return sy_log2(x); }
#define CEXP2(x) ctl_exp2(x)
#define CLOG2(x) ctl_log2(x)
#define CDB2LIN(x) ctl_exp2((x) * 0.166096405f)   /* (sy_db2lin: sy_exp2(db * 0.166096405f)) */
#else
#define CEXP2(x) sy_exp2(x)
#define CLOG2(x) sy_log2(x)
#define CDB2LIN(x) sy_db2lin(x)
#endif
SY_CTLI static float memo_exp2(float *c, float x) { if (c[0] != x) { c[0] = x; c[1] = CEXP2(x); } return c[1]; }
SY_CTLI static float memo_db2lin(float *c, float x) { if (c[0] != x) { c[0] = x; c[1] = CDB2LIN(x); } return c[1]; }
static void zero(void *p, uint32_t n) { uint8_t *b = p; while (n--) *b++ = 0; }
static void ctl_cache_reset(void);      /* (the control blocks' caches, before voice_block) */
#ifndef HYB_MULTI
static int arp_held;                    /* the keys the arpeggiator holds (arp_vel's non-zero count; ASSUMPTION: one
                                         * sy_synth per program, as the other statics here) */
static uint8_t cc_dirty;                /* a controller's smoothed value (ccs) may differ from its target (cc): set
                                         * where cc is changed, cleared when the glides have settled (sy_render) */
static uint32_t pv_gl[8];               /* bit i: pv[i] may differ from patch[i] (a superset: set wherever patch[i] is
                                         * written or pv[i] kept apart from it, cleared when it has settled, sy_render) */
#endif
#define PV_GL(i) (pv_gl[(unsigned)(i) >> 5] |= 1u << ((unsigned)(i) & 31u))
#ifdef HYB_MULTI                        /* (two parts: these three per part, in its sy_synth) */
#define arp_held (s->arp_held_m)
#define cc_dirty (s->cc_dirty_m)
#define pv_gl (s->pv_gl_m)
#define SY_NV(s) ((s)->nv)              /* the part's voices (its slice of the pool) */
static int insel_z_m[2] = {1, 1};       /* (sy_part_head.inc: a part's insel is all 0) */
#define SY_VI(s, v) ((int)((v) - (s)->v) + (s)->v0)   /* a voice's index in the pool (its control caches, vc) */
#else
#define SY_NV(s) SY_MAX_VOICES
#define SY_VI(s, v) ((int)((v) - (s)->v))
#endif

SY_COLD void sy_init(sy_synth *s, float fs)
{
    int i;
#ifdef HYB_MULTI
    {   /* (the part's memory elsewhere and its slice: kept, sy_multi_init set them) */
        sy_voice_t *v = s->v;
        sy_steal_t *st = s->steal;
        int nv = s->nv, v0 = s->v0, pidx = s->pidx;
        float *sl = s->sat_lut, (*ch)[1024 * 2] = s->ch_buf, (*tb)[128] = s->tables, *cm = s->cmap;
        int16_t *dl = s->dl_buf;
        float *rb = s->rv_buf, *rx = s->rvx_ring;
        zero(s, sizeof *s);
        s->v = v; s->steal = st; s->nv = nv; s->v0 = v0; s->pidx = (uint8_t)pidx; s->sat_lut = sl; s->ch_buf = ch;
        s->tables = tb; s->cmap = cm; s->dl_buf = dl; s->rv_buf = rb; s->rvx_ring = rx;
        s->send = -1;
        s->rt_g = 1.f;
    }
#else
    zero(s, sizeof *s);
#endif
    ctl_cache_reset();
    arp_held = 0;
    cc_dirty = 0;
    for (i = 0; i < 8; i++) pv_gl[i] = 0u;
    s->fs = fs;
    s->ifs = 1.f / fs;
    s->bdt = SY_BLOCK / fs;
    s->opt = SY_OPT_LATENCY;
    s->max_voices = SY_MAX_VOICES;
    s->arp_note = -1;
    s->channel = 0;
    sy_tables_init(s);
    ptab_init(s);
    for (i = 0; i < SY_NV(s); i++) s->v[i].rng = 0x9E3779B9u * (uint32_t)(i + 1);
    for (i = 0; i < 3; i++) {
        static const float PH0[3] = {LFO1_PH0, LFO2_PH0, LFO3_PH0};
        s->wt[i].id = -1;
        s->wt[i].stale = 0;
        s->glfo[i].rng = 12345u + (uint32_t)i;
        s->glfo[i].ph = PH0[i];
        s->glfo[i].sh_from = sy_randf(&s->glfo[i].rng);
        s->glfo[i].sh_to = sy_randf(&s->glfo[i].rng);
    }
    s->ch_rng = 777u;
    s->ch_sh_from = s->ch_sh_to = 0.5f;  /* the sample & hold starts in the middle */
    s->ch_hold = (int)(220.f * fs / REF_FS + 0.5f);   /* the chorus LFO starts 220 frames late (its phase
                                                     * read at three rates: 4.7 ms behind this one's) */
    s->stat.rng = 4242u;
    s->stat.lad[0] = 1e-5f;             /* (the seed, as in a voice) */
    s->sat_type_built = 255;
    s->bend = s->bend_t = 0.f;
    s->patch[PART_VOL] = 127;           /* the part volume starts at full (measured) */
    s->sm_dirty = 1;
    s->pv[PART_VOL] = 127.f;
    sy_fx_reset(s);
}

/* ---- parameters */
SY_CTLI static int wave_of(int w, int osc) { return w >= 65 || w < 0 || (osc == 2 && w == 64) ? 0 : w; }   /* (below 0: a
                                         * matrix offset on the wave; the sine, as above the range. Passed on, wave_at()
                                         * read a table of none: NULL + i on the device) */

static void waves_update(sy_synth *s)
{
    int i, id[3];
    int m3 = s->patch[B_O3_MODE];
    id[0] = wave_of(s->patch[O1_WAVE], 1);
    id[1] = wave_of(s->patch[O2_WAVE], 2);
    id[2] = m3 >= 6 ? wave_of(m3 - 4 > 64 ? 0 : m3 - 4, 1) : 0;
    for (i = 0; i < 3; i++)
        if (id[i] >= 2 && id[i] <= 63 && s->wt[i].id != id[i]) {
            sy_waves_build(&s->wt[i], id[i]);
            s->wt[i].id = (int16_t)id[i];
            s->wt[i].stale = 0;
        }
}

void sy_waves_stale(sy_synth *s, int id)
{
    int i;
    for (i = 0; i < 3; i++)
        if (s->wt[i].id == id) s->wt[i].stale = 1;   /* (it plays on as it is meanwhile) */
}

int sy_waves_rebuild(sy_synth *s, int id)
{
    int i;
    for (i = 0; i < 3; i++)
        if (s->wt[i].stale && s->wt[i].id == id) {
            sy_waves_build(&s->wt[i], id);
            s->wt[i].stale = 0;
            return 1;
        }
    return 0;
}

/* MEASURED: byte 0 of a patch is the version of the firmware that stored it. The
 * reference loads a patch of version 0-4 with the parameters that version did not have set to fixed values (the
 * stored bytes have no effect; a controller moving one afterwards does act). Bit-identical renders against the
 * reference after this table on every patch of a version 0-2 bank; version 5 and above (the factory banks: 7) load as
 * stored. {highest version it applies to, byte, value} */
static const uint8_t OLD_FIX[][3] = {
    {4, 38, 0},         {4, 39, 64},        {4, 112, 1},        {4, 119, 64},       /* ring mod, noise colour, delay mode, colour */
    {4, PB + 2, 0},     {4, PB + 5, 64},    {4, PB + 34, 0},    {4, PB + 41, 0},    /* arp pattern, note length, FM mode, osc 3 */
    {4, PB + 58, 0},                                                                /* B 58 */
    {4, PB + 80, 64},   {4, PB + 82, 64},   {4, PB + 85, 0},    {4, PB + 92, 64},   /* LFO 1/2 assign amounts, phaser, EQ */
    {4, PB + 95, 64},   {4, PB + 96, 64},   {4, PB + 97, 0},    {4, PB + 99, 0},    /* EQ, boost, input ring modulator */
    {4, PB + 103, 0},   {4, PB + 106, 0},   {4, PB + 109, 0},                       /* matrix slots 4-6 */
    {1, 109, 64},                                                                   /* chorus feedback */
    {0, PB + 13, 0},    {0, PB + 16, 57},   {0, PB + 18, 0},    {0, PB + 19, 0},    /* LFO 3 fade, tempo, LFO 1/2 clock */
    {0, PB + 20, 0},    {0, PB + 33, 36}};                                          /* delay clock, filter keytrack base */

SY_COLD void sy_load_patch(sy_synth *s, const uint8_t *p)
{
    s->loaded = 1;
    int i;
    s->sat_type_built = 255;             /* (the saturation's table built again: a table gone wrong ends here) */
    uint8_t part = s->patch[PART_VOL];
    float part_v = s->pv[PART_VOL];
    for (i = 0; i < 256; i++) { s->patch[i] = p[i]; s->pv[i] = (float)p[i]; s->sm_inc[i] = 0.f; s->sm_seen[i] = 0; }
    for (i = 0; i < (int)(sizeof OLD_FIX / sizeof OLD_FIX[0]); i++)
        if (p[0] <= OLD_FIX[i][0]) { s->patch[OLD_FIX[i][1]] = OLD_FIX[i][2]; s->pv[OLD_FIX[i][1]] = (float)OLD_FIX[i][2]; }
    s->sm_dirty = 1;
    s->fx_wake = 1;
    for (i = 0; i < SY_FXN; i++) s->fxm[i] = 0.f;   /* MEASURED: a patch load ends the effects' held matrix offsets */
    s->rm_off = s->dry_off = 0;         /* (the part's, as the effects') */
    for (i = 0; i < 9; i++) s->eq_key[i] = 255;   /* (the EQ takes the patch's values at once: no glide, sy_fx.c eq_glide) */
    s->fx_mod = 0;
    s->fx_snap = 1;                     /* (the effects take the new patch's values at once: no ramp from the old one's) */
#ifdef HYB_SHIMMER
    s->shm[0] = s->shm[1] = s->shm[2] = 0.f;   /* (the shimmer's, as the effects') */
#endif
    for (i = 0; i < 128; i++) s->cc_seen[i] = 0;
    s->bend_seen = 0;
    /* the part volume byte of a patch has no effect: the part volume follows CC 7 only and keeps its value across
     * patch changes (measured) */
    s->patch[PART_VOL] = part;
    s->pv[PART_VOL] = part_v;
    PV_GL(PART_VOL);                    /* (the only entry the load may leave apart) */
    waves_update(s);
}

/* one block of a controller's glide (inter.json cc_speed, smooth mode 1): the volumes exponential (13.6 ms), the
 * rest a straight line in a fixed time, whatever the step (read on the pan and the cutoff with steps of 4 to
 * 127: 1890 frames from 2 % to 98 %, 42 ms); a controller's first move after the patch load is a step */
#define SM_TIME 0.042f
SY_CTLI static float glide(float x, float tgt, float *inc, uint8_t *last, int t, float bdt)
{
    if (*last != t || *inc == 0.f) { *inc = (tgt - x) * bdt * (1.f / SM_TIME); *last = (uint8_t)t; }
    x += *inc;
    if ((*inc > 0.f && x >= tgt) || (*inc < 0.f && x <= tgt)) { x = tgt; *inc = 0.f; }
    return x;
}

static void smooth_step(sy_synth *s, int i)
{
    if (i == PART_VOL || i == PATCH_VOL || i == 113) {
        float d = (float)s->patch[i] - s->pv[i];
        s->pv[i] += d * (1.f - sy_exp2(-s->bdt / 0.0136f * 1.44269504f));
        if (sy_absf((float)s->patch[i] - s->pv[i]) < 0.01f) s->pv[i] = (float)s->patch[i];
        return;
    }
    s->pv[i] = glide(s->pv[i], (float)s->patch[i], &s->sm_inc[i], &s->sm_tgt[i], s->patch[i], s->bdt);
}

/* CLEANER (GLO > AUDIO > FILT KNOBS SMOOTH, s->flt_sm; a clean option): a new
 * base value of cutoff 1, 2 or resonance 1, 2 (A 40-43: a knob, a controller, the remote editor; sy_set_param) glides
 * from the value now to the new one in a straight line instead of stepping at the block's start. Its time: the blocks
 * since that byte's last new target while they are FS_NC or fewer (a turn: each move over the time to the next one,
 * the moves joined into one line, a move behind; a jump, more than FS_JUMP steps, only up to FS_NCJ), at least
 * FS_NMIN; after a pause a single step FS_NS, a jump FS_NJ. The patch's smooth mode (B 25 odd) keeps its own glide (smooth_step), the first move
 * after a load gliding too. A patch load, a note, the envelopes, LFOs, matrix, keytrack and velocity are not touched
 * (the base value alone: pv). Measured on a held note (tools/measure/filt_knobs.py) */
#ifndef FS_NMIN
#define FS_NMIN 2u                      /* (blocks: 5.5 ms at 46.9 kHz) */
#endif
#ifndef FS_NC
#define FS_NC 200u                      /* (546 ms: a slow turn's detents joined into one line up to ~2 a second; 1.18C35
                                         * 37, 101 ms: slower turns a staircase of 44 ms glides and plateaus, the user's
                                         * device test; clean-cutoff.md "1.18C36") */
#endif
#ifndef FS_NCJ
#define FS_NCJ 37u                      /* (101 ms: a jump, more than FS_JUMP steps, joined to the moves before it only
                                         * this close, else FS_NJ: as 1.18C35's FS_NC for every move) */
#endif
#ifndef FS_NS
#define FS_NS 48u                       /* (131 ms; 1.18C35 16, 43.7 ms) */
#endif
#ifndef FS_NJ
#define FS_NJ 4u                        /* (10.9 ms) */
#endif
#ifndef FS_JUMP
#define FS_JUMP 2.5f
#endif
static SY_COLD __attribute__((noinline)) int flt_glide(sy_synth *s)   /* one block (part head): 2 still gliding, 1 the
                                         * last move this block, 0 none */
{
    int q, left = 0, moved = 0;
    if (!(s->patch[B_SMOOTH] & 1u))     /* (the smooth mode: its own glide, smooth_step) */
    for (q = 0; q < 4; q++) {
        const int i = CUT1 + q;
        const float t = (float)s->patch[i];
        float x = s->pv[i], inc = s->sm_inc[i];
        if (x == t) { s->sm_inc[i] = 0.f; continue; }
        if (s->sm_tgt[i] != s->patch[i] || inc == 0.f) {   /* (a new target: a new line from the value now) */
            uint32_t n = (uint16_t)((uint16_t)s->block - s->fs_t[q]);
            const float d = t - x;
            if (sy_absf(d) > FS_JUMP) { if (n > FS_NCJ) n = FS_NJ; }   /* (a jump: joined to a turn's moves up to FS_NCJ,
                                         * else fast) */
            else if (n > FS_NC) n = FS_NS;      /* (a single step after a pause) */
            if (n < FS_NMIN) n = FS_NMIN;
            inc = d / (float)n;
            s->sm_tgt[i] = s->patch[i];
            s->fs_t[q] = (uint16_t)s->block;
        }
        x += inc;
        if ((inc > 0.f && x >= t) || (inc < 0.f && x <= t)) { x = t; inc = 0.f; }
        s->pv[i] = x;
        s->sm_inc[i] = inc;
        moved = 1;
        left |= x != t;
    }
#if FS_PG
    for (q = 0; q < SY_NV(s); q++)      /* (the voices that sounded before spread the passband gain's change over the
                                         * block: sy_voice.c vr_filt; a voice's first block steps) */
        s->v[q].pr = (uint8_t)(moved && s->v[q].started);
#endif
    return left ? 2 : moved;
}

static void arp_switch(sy_synth *s, int on);
#ifdef SY_UNI_CAP
static SY_COLD void uni_live(sy_synth *s);
#ifndef UNI_LIVE
#define UNI_LIVE 1
#endif
#endif
SY_COLD void sy_set_param(sy_synth *s, int i, int val)
{
    int live = 0;                       /* (UNISON changing held notes: their levels ramp, no step) */
    if (i < 0 || i > 255) return;
    if (val < 0) val = 0;
    if (val > 127) val = 127;
    if (i == B_ARP && !val != !s->patch[B_ARP]) arp_switch(s, val);
#ifdef SY_UNI_CAP
    if (UNI_LIVE && i == UNISON && val != s->patch[UNISON]) { s->patch[i] = (uint8_t)val; uni_live(s); live = 1; }
#endif
    s->patch[i] = (uint8_t)val;
    s->sm_dirty = 1;
    s->fx_wake = 1;
    PV_GL(i);
    if (s->flt_sm && (unsigned)(i - CUT1) < 4u) {   /* (FILT KNOBS SMOOTH: cutoff 1, 2, resonance 1, 2 glide) */
        if (!(s->patch[B_SMOOTH] & 1u)) s->fs_act = 1;   /* (flt_glide; the smooth mode: smooth_step) */
    } else
    if ((!(s->patch[B_SMOOTH] & 1u) || !s->sm_seen[i]) && s->pv[i] != (float)val) {
        s->pv[i] = (float)val;
        s->sm_inc[i] = 0.f;
        if (!live) s->pv_jump = 1;      /* a step at the block's start (inter.json cc_speed: no ramp) */
    }                                   /* else it glides from this block on (sy_render) */
    s->sm_seen[i] = 1;
    if (i == O1_WAVE || i == O2_WAVE || i == B_O3_MODE) waves_update(s);
}

/* ---- MIDI: queued for the block latency, applied at the start of a block */
void sy_midi(sy_synth *s, uint8_t st, uint8_t d1, uint8_t d2)
{
    sy_event_t *e;
    if (s->ev_w - s->ev_r >= SY_EVQ) return;
    e = &s->evq[s->ev_w % SY_EVQ];
    e->at = s->block + ((s->opt & SY_OPT_LATENCY) ? (SY_PIPE_ON(s) ? 1u : 2u) : 0u);   /* (the effects' block later: one less here) */
    e->st = st; e->d1 = d1; e->d2 = d2;
#ifdef __GNUC__
    __asm__ __volatile__("" ::: "memory");   /* the event is complete before the render (an interrupt) sees it */
#endif
    s->ev_w++;
}

void sy_set_param_queued(sy_synth *s, int i, int val)
{
    if (i < 0 || i > 255) return;
    sy_midi(s, SY_EV_PARAM, (uint8_t)i, (uint8_t)(val < 0 ? 0 : val > 127 ? 127 : val));
}

static void env_start(sy_env_t *e)
{
    e->stage = 1;
    e->an = 0.f;
    e->al0 = e->lvl;
}

static void lfo_trigger(sy_lfo_t *l)
{
    l->ph = 0.f;
    l->done = 0;
}

/* a free voice: the released ones first (the quietest), then the oldest held one */
#ifndef MONO_KEEP_FILT
#define MONO_KEEP_FILT 1                /* a mono note on a voice still in its release (voice_start keep): the
                                         * filters run on, the analog modes' ladder too (read: the reference has no
                                         * step there; reset to 0 while the amp went on, a low cutoff clicked at every
                                         * staccato note) */
#endif
#ifndef MONO_KEEP_LVL
#define MONO_KEEP_LVL 0.5f              /* a mono note goes on with the releasing voice (above) only while its amp
                                         * envelope is above this (-6 dB, absolute); below, the note takes a new voice
                                         * and the old one fades out in a block (read: a self-oscillating
                                         * mode 7 without input, the next note 1-12 blocks into releases 8-60 at sustain
                                         * 60 / 100 / 127: the reference's oscillation runs on above ~-6 dB; below, the
                                         * old voice is gone within ~2 ms and the new one builds up afresh. Run
                                         * on below that, a self-oscillating ladder stepped at every staccato note: 00
                                         * acid saw 430-730x the held note's curvature against the reference's 11;
                                         * PROVENANCE.md 12b) */
#endif
#ifndef MONO_KEEP_LAT
#define MONO_KEEP_LAT 0.5f              /* (blocks ahead: the release's level when the note sounds) */
#endif
#ifndef MONO_KEEP_SAT
#define MONO_KEEP_SAT 1                 /* the saturation's states run on too (read: types 7, 11-14 reset gave a
                                         * step the reference has not; types 8-10 run on as well) */
#endif
#ifndef MONO_KEEP_FENV
#define MONO_KEEP_FENV 1                /* the filter envelope goes on from its level like the amp envelope
                                         * (read: filter env attack 30 / 60, reset to 0 is far off the reference) */
#endif
#ifndef MONO_KEEP_CLK
#define MONO_KEEP_CLK 1                 /* no new start transient (punch) on such a note (read: punch 64 / 127,
                                         * the reference's retrigger is as smooth as a legato note's) */
#endif
#ifndef LAD_Q_ON
#define LAD_Q_ON 1                      /* mode 7's self-oscillation is not quenched by an input above the cutoff
                                         * as a loop saturating at its input is: from resonance LAD_Q_R0 to LAD_Q_R1 the
                                         * input stage's knee rises by LAD_Q_LCM and stage 2's input takes the limiting
                                         * (scale LAD_Q_SS1); see PROVENANCE.md "mode 7's self-oscillation under input" */
#endif
#ifndef LAD_Q_R0
#define LAD_Q_R0 84.f
#endif
#ifndef LAD_Q_R1
#define LAD_Q_R1 96.f
#endif
#ifndef LAD_Q_LCM
#define LAD_Q_LCM 1000.f
#endif
#ifndef LAD_Q_DG
#define LAD_Q_DG 1.4372f                /* stage 2's scale rises with 0.15 of the drive (LAD_DRV): x1.437 = +3.15 dB at osc
                                         * volume 127 */
#endif
#ifndef LAD_Q_SS1
#define LAD_Q_SS1 0.63f
#endif
#ifndef LAD_STS_B
#define LAD_STS_B 1.2f
#endif
#ifndef LAD_STS_H
#define LAD_STS_H 1.6f
#endif
#ifndef LAD_STS_R0
#define LAD_STS_R0 80.f
#endif
#ifndef LAD_STS_R1
#define LAD_STS_R1 108.f
#endif
#ifndef SAT5_GF
#define SAT5_GF 1.f
#endif
#ifndef VC6_GAIN
#define VC6_GAIN 1
#endif
#ifndef RING_SUB
#define RING_SUB 1                      /* the sub in the ring modulator's first factor (below; 0: the earlier product) */
#endif
#ifndef BAL_LAW
#define BAL_LAW 1                       /* the filter balance as measured (synth.c); 0: the earlier straight law */
#endif
#ifndef DET_WANDER
#define DET_WANDER 1                    /* CLEANER: osc 2's (and osc 3's) detune wanders (synth.c dw_tick; PROVENANCE 9e, 9f) */
#endif
#ifndef V6_LOAD
#define V6_LOAD 1                       /* CLEANER: the 6-voice state also from the part's load (v6_on; PROVENANCE 9g) */
#endif
#ifndef CO_V6
#define CO_V6 1                         /* CLEANER: the oscillators' 6-voice state (synth.c, sy_cln_osc.inc; PROVENANCE 9d) */
#endif
#ifndef FILT2_TAB
#define FILT2_TAB 1                     /* filter 2 per cutoff as filter 1 (synth.c filt_coef) */
#endif
#ifndef HYB_OSC_TICK
#define HYB_OSC_TICK 1                  /* osc 1 and 2 on the reference's tick clock (sy_voice.c) */
#endif
#ifndef ENV_END
#define ENV_END 4e-5f
#endif
#ifndef LEG_FRESET
#define LEG_FRESET 0
#endif
#ifndef ARP_KEEP_PREV
#define ARP_KEEP_PREV 1
#endif
#ifndef ARP_MARK
#define ARP_MARK 1.18f
#endif
#ifndef ARP_KB
#define ARP_KB 1u
#endif
#ifndef LEGATO_RETRIG
#define LEGATO_RETRIG 1
#endif
#ifndef STEAL_LVL
#define STEAL_LVL 0
#endif
#ifndef POLY_LOAD
#define POLY_LOAD 1
#endif
/* the reference's voice limit follows its processing load: a budget shared by voices whose cost depends on
 * the patch (read: notes held one after another, the first one's line in the spectrum; init patch 32 voices, filter
 * 1 analog 20; osc 2 heard 18, saturation or noise 19, ring modulator 17, routing 1 19 and 3 15 (digital: 3 with
 * osc 2 26), the reverb modes 2-4 19 (at any send); sub osc, waves, FM, sync, chorus, resonance, envelopes, LFOs, the matrix and the
 * effects' other modes cost nothing measurable; a unison voice counts as a voice; the trance pad (analog, osc 2,
 * saturation 1, effects 3) 16). The costs below fit every reading within one voice. */
static SY_NSL int patch_poly(const sy_synth *s)
{
    const uint8_t *p = s->patch;
    int analog = p[F1_MODE] == 5 || p[F1_MODE] == 7, o2 = p[OSC_BAL] > 0 || p[RING_VOL] > 0, n;
    float c = analog ? 0.0488f : 0.0310f, b = (p[112] >= 2 && p[112] <= 4) ? 0.95f : 1.f;   /* (the reverb modes 2-4, at any send) */
    if (o2) c += 0.0055f;
    if (p[RING_VOL]) c += 0.0028f;
    if (p[SAT_TYPE]) c += 0.0026f;
    if (p[NOISE_VOL]) c += 0.0026f;
    if (p[ROUTE] == 1) c += 0.0026f;
    if (p[ROUTE] == 3) c += analog ? 0.0157f : 0.0015f;
    n = (int)(b / c);
    return n < 1 ? 1 : n;
}

/* CLEANER: the 6-voice state (VC6_GAIN, CO_V6) is on with 6 voices or more, or earlier when the part's load reaches
 * V6_L (measured, black box: PROVENANCE 9g; one audible voice with k silent companions, the reference's
 * audio alone): a voice's cost in units, 3 plus 2 for each of osc 2 heard (as patch_poly), routing 1, a saturation
 * type, a matrix slot with a voice's source (19 and up: the envelopes, the LFOs, velocity, keyfollow, the constant;
 * pitch bend, pressure and the controllers cost nothing), the noise, the ring modulator, osc 3, and 7 for the analog
 * filter modes, times the voices on; plus 13 for the reverb (modes 2-4), 8 for the EQ (a band away from 64 or the
 * boost) and V6_PH for the phaser (its mix above 0; read between 9.5 and 10.5). The units are one solution with
 * the largest margin (2.75 %) that orders the thresholds read (2-6 voices); noise, ring and osc 3 were seen to cost
 * (one reading each), their 2 is an ASSUMPTION, as are routing 2 (0) and part B's load (its own). Filter 1's mode 6
 * (bypass) and routing 3 (split) never take the state (init saw and a test patch, 1-14 voices: neither the darker highs nor the gain) */
#ifndef V6_L
#define V6_L 54.5f
#endif
#ifndef V6_PH
#define V6_PH 10.f
#endif
static SY_LATE int v6_on(const sy_synth *s)
{
    const uint8_t *p = s->patch;
    float c = 3.f, f;
    int i;
    if (V6_LOAD && (p[F1_MODE] == 6 || p[ROUTE] == 3)) return 0;   /* (filter 1 bypassed or the split routing: never,
                                         * 1-14 voices, its +0.55 dB neither) */
    if (s->n_on >= 6) return 1;
    if (!V6_LOAD || !s->n_on) return 0;
    if (p[F1_MODE] == 5 || p[F1_MODE] == 7) c += 7.f;
    if (p[OSC_BAL] > 0 || p[RING_VOL] > 0) c += 2.f;
    if (p[ROUTE] == 1) c += 2.f;
    if (p[SAT_TYPE]) c += 2.f;
    if (p[NOISE_VOL]) c += 2.f;
    if (p[RING_VOL]) c += 2.f;
    if (p[B_O3_MODE]) c += 2.f;
    static const uint8_t SRC[6] = {64, 67, 72, 103, 106, 109};   /* (the slots' sources; 4-6 as the matrix reads them) */
    for (i = 0; i < 6; i++)
        if (p[PB + SRC[i]] >= 19) { c += 2.f; break; }
    f = (p[112] >= 2 && p[112] <= 4 ? 13.f : 0.f) + (s->eq_on ? 8.f : 0.f) + (p[PB + 85] ? V6_PH : 0.f);
    return (float)s->n_on * c + f >= V6_L;
}

#define STEAL_KEEP 1                    /* (CLEANER: measured, see PHASE_TAB) */
#ifdef SY_UNI_CAP
/* UNISON FIRST (uni_steal): with no released voice to take, an extra unison voice of a held note, where it is heard
 * least: the lowest held note (the root) and the highest (the top voice) are kept, an inner note gives first (the one
 * with the most unison voices, then the oldest); with only the outer two left, the highest before the lowest. A note
 * keeps one voice at least. Of its voices the one nearest the centre of its detune and spread goes (the centre, then
 * the inner pair, the second of a pair first): the outer pair, the note's width, stays, and a note that gives two
 * voices keeps its balance. -1: none (then a whole held note, as always). The voice fades out while the note's other
 * voices take over its level (uni_fade). A note whose voices wait (uni_pend) gives none */
static SY_NSL int uni_victim(const sy_synth *s)
{
    int i, j, k, lo = 128, hi = -1, best = -1, bn = 1, bc = 3;
    uint32_t ba = 0xFFFFFFFFu;
    for (i = 0; i < SY_NV(s); i++)      /* (the held notes' outer keys, the notes starting now too) */
        if (s->v[i].on && s->v[i].gate) {
            if (s->v[i].key < lo) lo = s->v[i].key;
            if (s->v[i].key > hi) hi = s->v[i].key;
        }
    for (i = 0; i < SY_NV(s); i++) {
        const sy_voice_t *v = &s->v[i];
        int n = 0, last = i, cls;
        uint32_t age = v->age;
        if (!(v->on && v->gate && v->uni_n > 1) || v->fresh) continue;   /* (not a note starting now) */
        for (k = 0; k < s->upn; k++) if (s->upend[k].key == v->key) break;
        if (k < s->upn) continue;
        for (j = 0; j < SY_NV(s); j++) {   /* (the note's voices: its key and unison count, held) */
            const sy_voice_t *w = &s->v[j];
            if (w->on && w->gate && w->key == v->key && w->uni_n == v->uni_n) {
                int n1 = w->uni_n & 1, r = (w->uni_i + n1) >> 1, rl = (s->v[last].uni_i + n1) >> 1;
                n++;                    /* (r: the slot's distance from the centre in pairs, synth.c uni_x) */
                if (w->age < age) age = w->age;
                if (r < rl || (r == rl && w->uni_i > s->v[last].uni_i)) last = j;
            }
        }
        if (n < 2) continue;
        cls = v->key == lo ? 2 : v->key == hi ? 1 : 0;   /* (0 inner, 1 the top, 2 the root: the lower class first) */
        if (cls < bc || (cls == bc && (n > bn || (n == bn && age < ba)))) { bc = cls; bn = n; ba = age; best = last; }
    }
    return best;
}
#endif
#ifdef HYB_MULTI
/* two parts, DYNAMIC (synth.h sy_multi_t dyn): the slices' boundary moves. A part needing a voice takes a free one of
 * its own while the voices on in all are under the cap, else a free one of the other part (the boundary moves a voice
 * that way); with the pool full, a part under its share takes the oldest released (else the oldest held) voice of the
 * other part if that one is over its share, else it steals from itself as always */
static sy_multi_t *mx_m;                /* (the setup the parts belong to: sy_multi_init) */
static int mx_on_n(const sy_synth *s)
{
    int i, n = 0;
    for (i = 0; i < s->nv; i++) n += s->v[i].on;
    return n;
}
static void mx_move(sy_synth *to)       /* a free voice of the other part to the boundary, then to part to */
{
    sy_synth *a = mx_m->p[0], *b = mx_m->p[1], *from = to == a ? b : a;
    int i, e = to == a ? 0 : from->nv - 1;
    if (from->nv < 1) return;
    if (from->v[e].on) {
        for (i = 0; i < from->nv && from->v[i].on; i++)
            ;
        if (i == from->nv) return;
        { sy_voice_t t = from->v[e]; from->v[e] = from->v[i]; from->v[i] = t; }
    }
    from->v[e].used = 0;                /* (no phase hand-over across the parts) */
    from->v[e].solo = 0;
    if (to == a) { a->nv++; b->v++; b->v0++; b->nv--; }
    else { a->nv--; b->v--; b->v0--; b->nv++; }
    a->max_voices = a->nv;
    b->max_voices = b->nv;
}
static int mx_share(const sy_multi_t *m, int pidx)   /* a part's share of the cap (at least 2: its guarantee) */
{
    int c = m->cap_now, sa = (c * m->nv_now + SY_MAX_VOICES / 2) / SY_MAX_VOICES;
    if (sa < 2) sa = 2;
    if (sa > c - 2) sa = c - 2;
    return pidx ? c - sa : sa;
}
static int mx_room(sy_synth *s)         /* before part s takes a voice: 1 = it may take a free one of its slice */
{
    sy_multi_t *m = mx_m;
    sy_synth *o;
    int on_s, on_o, i;
    if (!m || !m->dyn_now || m->mode_now == SY_M_SINGLE) return 1;
    o = m->p[s->pidx ^ 1];
    on_s = mx_on_n(s); on_o = mx_on_n(o);
    if (on_s + on_o < m->cap_now) {
        for (i = 0; i < s->nv; i++) if (!s->v[i].on) return 1;
        if (on_o < o->nv) { mx_move(s); return 1; }
        return 0;
    }
    if (on_s < mx_share(m, s->pidx) && on_o > mx_share(m, o->pidx)) {   /* the other part over its share: its voice */
        int vi = -1;
        uint32_t best = 0xFFFFFFFFu;
        for (i = 0; i < o->nv; i++)
            if (o->v[i].on && !o->v[i].gate && o->v[i].age < best) { best = o->v[i].age; vi = i; }
        if (vi < 0)
            for (i = 0; i < o->nv; i++)
                if (o->v[i].on && o->v[i].age < best) { best = o->v[i].age; vi = i; }
        if (vi >= 0) {
            o->v[vi].on = o->v[vi].gate = 0;
            o->v[vi].aenv.stage = o->v[vi].fenv.stage = 0;
            mx_move(s);
            return 1;
        }
    }
    return 0;
}
#define MX_ULIM(s) (mx_m && mx_m->dyn_now && mx_m->mode_now != SY_M_SINGLE ? mx_m->cap_now : (s)->max_voices)
#endif
/* REL_FADE (1.18C48, not the reference's: there a released voice taken for a new note is cut at once): with no voice
 * free over the cap, a released voice still audible (its amp envelope over REL_FADE_LVL) fades out (REL_FADE 2: ~15 ms,
 * uni_fade_b; 1: one block) and the new note's voice waits for it (uni_wait, uni_pend: the voice starts when the fade
 * ends), as a held note's unison voice already does. A long release cut at -12 dB clicked (25-40 dB over the high band's
 * floor with the filter half closed). 0: the hard cut, as before. Both allocations (UNISON FIRST, NOTES) */
#ifndef REL_FADE
#define REL_FADE 2
#endif
#ifndef REL_FADE_LVL
#define REL_FADE_LVL 0.01f
#endif
#define REL_FADE_NB(s) (REL_FADE == 1 ? 1 : uni_fade_b(s))
#if REL_FADE && defined(SY_UNI_CAP)
#define REL_FADING(v) ((v)->ufade)
static SY_NSL int uni_fade_b(const sy_synth *s);
static SY_NSL void uni_fade(sy_synth *s, sy_voice_t *w, int nb, int comp);
#else
#define REL_FADING(v) 0
#endif
static SY_NSL sy_voice_t *alloc_voice(sy_synth *s)
{
    int i, best = -1, n_on = 0;
    float bl = 1e9f;
    uint32_t oldest = 0xFFFFFFFFu;
#ifdef HYB_MULTI
    int room = mx_room(s);
    for (i = 0; i < s->max_voices; i++) n_on += s->v[i].on;
    if ((!POLY_LOAD || n_on < patch_poly(s)) && room)
        for (i = 0; i < s->max_voices; i++)
            if (!s->v[i].on) return &s->v[i];
#else
    for (i = 0; i < s->max_voices; i++) n_on += s->v[i].on;
    if (!POLY_LOAD || n_on < patch_poly(s))
        for (i = 0; i < s->max_voices; i++)
            if (!s->v[i].on) return &s->v[i];
#endif
    for (i = 0; i < s->max_voices; i++)   /* of the released voices the oldest (read on an arp of unison
                                         * chords at the voice limit: the earliest steps' voices go first) */
        if (s->v[i].on && !s->v[i].gate && !REL_FADING(&s->v[i]) && (STEAL_LVL ? s->v[i].aenv.lvl < bl : (float)s->v[i].age < bl)) {
            bl = STEAL_LVL ? s->v[i].aenv.lvl : (float)s->v[i].age; best = i;
        }
#if REL_FADE && defined(SY_UNI_CAP)
    if (best >= 0 && s->v[best].aenv.lvl > REL_FADE_LVL) {   /* (1.18C48: an audible release tail fades, the new
                                         * voice waits for it: note_voice's caller, uni_wait) */
        uni_fade(s, &s->v[best], REL_FADE_NB(s), 0);
        return 0;
    }
#endif
    if (best < 0) {
        for (i = 0; i < s->max_voices; i++)
            if (s->v[i].on && s->v[i].age < oldest) { oldest = s->v[i].age; best = i; }
        if (best >= 0 && STEAL_KEEP && s->steal_n < 16)   /* a held voice taken keeps its phases too, once
                                         * the note's voices are started (read in the reference: the oldest held voice
                                         * is ended after them and put on the stack) */
            SY_STEAL_PUT(&s->steal[s->steal_n++], &s->v[best]);
    }
    if (best < 0) best = 0;
#ifdef PTAB_DEBUG
    fprintf(stderr, "steal blk %u voice %d gate %d age %u\n", s->block, best, s->v[best].gate, s->v[best].age);
#endif
    return &s->v[best];
}

#ifndef UNI_SPREADPH
#define UNI_SPREADPH 0
#endif
#ifndef UNI_PHJ
#define UNI_PHJ 0.27f
#endif
#ifndef REL_PH
#define REL_PH 1
#endif
#ifndef SLIDE_SOLO
#define SLIDE_SOLO 0
#endif
/* the tuning offset. Free running (B 35 = 0) each note carries the stretch x a step u of a fixed cycle of 16
 * (TUNE_STEP), one step per note-on whatever the key (a chord's tones take the next steps in their order, a note's
 * unison voices share one; both oscillators alike; the first note after load takes u = 1). With an osc init phase
 * v > 0 every note is at u = v/127 and the stretch reads 0.19 c + stretch x u. (read: 16-64 notes on one key at the
 * 16th harmonic, keys 48-96, 1-3 tone chords, unison 2; the steps evenly spaced from -0.89 to 1) */
static const float TUNE_STEP[16] = {1.f, -0.250f, 0.383f, -0.886f, -0.127f, 0.620f, -0.630f, 0.130f,
    0.886f, -0.373f, 0.506f, -0.753f, 0.006f, 0.763f, -0.506f, 0.244f};


#ifndef UNI_PAIRS
#ifndef ARP_CLK
#define ARP_CLK 1
#endif
#ifndef ARP_CLK_OFF
#define ARP_CLK_OFF 0.912f                /* the tick clock's phase against the load (ticks), and its */
#endif
#ifndef ARP_CLK_B
#define ARP_CLK_B -1.5f                 /* offset in blocks (read on the first step at tempos 30 / 64 / 100: 58 of 60 to the block) */
#endif
#ifndef ARP_PC
#define ARP_PC 1                        /* an arp note sounds 1 block after its step */
#endif
#ifndef LFO_CLK_LAG
#define LFO_CLK_LAG 3.5f
#endif
#ifndef ARP_LATE
#define ARP_LATE 1
#endif
#ifndef EV_ONS_FIRST
#define EV_ONS_FIRST 1
#endif
static uint8_t off_pre;                 /* CLEANER (1.18C34 fix, sy_part_head.inc): the note-off applied now was queued
                                         * before a note-on of its key in this block: it ends the voices of its key
                                         * started before its place in the queue alone (their age up to off_lim), never
                                         * a note-on's after it (that one may have taken the old note's voice) */
static uint32_t off_lim;
/* CLEANER (not the reference's; a platform's choice): a chord's voice starts spread over a few blocks. A block that
 * starts many voices at once (a big chord, unison) costs far more than the blocks around it (the new voices' set-up
 * and first blocks, the steals' fades; on the FM-1 its flash code cold): with LATENCY LOW such a block ran past its
 * time (a dropout, a click) before the overload guard could act. START_SPREAD: once a
 * block's events have started sy_start_budget voices (both parts together; a key's unison voices always together),
 * its next note-on and every event behind it wait in the queue for the next block, in their order (a note-off of a
 * key whose note-on waits comes after it: the note still starts, then ends, a short note), at most START_LATE_MAX
 * blocks: an event that late is taken whatever the count. MIDI's real-time messages (clock, start, stop) behind a
 * waiting note-on are taken at once (the tempo the ticks measure not shifted). Not with the queue half full (no event
 * dropped for the wait). A chord within the budget, single notes, the arpeggiator's notes: as before, to the bit.
 * 0: as before (bit-identical) */
#ifndef START_SPREAD
#define START_SPREAD 1
#endif
#ifndef START_BUDGET
#define START_BUDGET 8                  /* the voice starts a block takes (sy_start_budget's default) */
#endif
#ifndef START_LATE_MAX
#define START_LATE_MAX 4                /* blocks a note-on waits at most (4: 11 ms at 46,875 Hz) */
#endif
volatile uint8_t sy_start_budget = START_BUDGET;
#if START_SPREAD
static uint32_t spr_used;               /* the voices started this block (sy_part_head.inc; part A's head clears it) */
static void apply_event(sy_synth *s, const sy_event_t *e);
/* the starts part s's next note-on may follow: the budget; two parts (LAYER, SPLIT, CHANNELS) part A half of it while
 * part B has a note-on due (B's head comes after A's: B takes what A left, not nothing) */
SY_COLD static uint32_t spr_lim(const sy_synth *s)
{
    uint32_t b = sy_start_budget;
#ifdef HYB_MULTI
    if (!s->pidx && mx_m && mx_m->mode_now != SY_M_SINGLE) {
        const sy_synth *o = mx_m->p[1];
        uint32_t r;
        for (r = o->ev_r; r != o->ev_w; r++) {
            const sy_event_t *e = &o->evq[r % SY_EVQ];
            if ((int32_t)(e->at - o->block) > 0) break;
            if ((e->st & 0xF0) == 0x90 && e->d2) { b = (b + 1u) / 2u; break; }
        }
    }
#else
    (void)s;
#endif
    return b;
}
/* the real-time messages due behind a waiting note-on: taken now, their entries left as 0xF9 (ignored) */
SY_COLD static void spr_rt(sy_synth *s)
{
    uint32_t r;
    for (r = s->ev_r; r != s->ev_w; r++) {
        sy_event_t *e = &s->evq[r % SY_EVQ];
        if ((int32_t)(e->at - s->block) > 0) break;
        if (e->st >= 0xF8 && e->st <= 0xFC && e->st != 0xF9) {
            apply_event(s, e);
            e->st = 0xF9;
        }
    }
}
#endif
#ifndef SYNC_PH0
#define SYNC_PH0 1                      /* under sync osc 2 restarts at detune / 128 of a cycle (read
                                         * on synced saws, semi 0-24, detune 0-127: harmonics within 0.1 dB) */
#endif
#define UNI_PAIRS 1
#endif
/* (read per voice line) a note's unison voices in the order they are started: an odd count
 * starts with the centre, then pairs, each pair's first at +p/P of the detune and its second at -p/P (P pairs, p =
 * 1 .. P). The centre and each pair take a tuning step of their own (so a note takes (n + 1)/2 steps). */
SY_CTLI static float uni_x(int n, int i)
{
    int P, p;
    if (n < 2) return 0.f;
    if (!UNI_PAIRS) {
        int j = i, m = n;
        if (!(n & 1)) { m = n + 1; if (j >= n / 2) j++; }
        return 2.f * (float)j / (float)(m - 1) - 1.f;
    }
    if (n & 1) { if (!i) return 0.f; P = (n - 1) / 2; p = (i + 1) / 2; return (i & 1 ? 1.f : -1.f) * (float)p / (float)P; }
    P = n / 2; p = i / 2 + 1;
    return (i & 1 ? -1.f : 1.f) * (float)p / (float)P;
}
static int uni_step(int n, int i) { return (n & 1) ? (i + 1) / 2 : i / 2; }
#ifndef UNI_TAPER
#define UNI_TAPER 2.5f                  /* (the unison voices' level per slot: voice_block) */
#endif
#ifndef UNI_DB
#define UNI_DB -2.4f
#endif

#ifdef SY_UNI_CAP
/* A note's loudness with fewer unison voices than its patch asks (the caps at its start, uni_cap; a voice taken by
 * UNISON FIRST, uni_fade): detuned voices add in power, so the note's voices are raised by sqrt(P / p), P the power
 * of the patch's count (each slot's level as voice_block tapers it, its pan law at the patch's pan and spread), p of
 * the slots it plays (the voices waiting for a voice, uni_pend, counted as playing). Its spread stays: the voices
 * keep their slots. The correction (v->ug) ramps in uni_fade_b blocks after a steal; at a note's start it is there at
 * once. With the caps off, nothing is taken from a note: the correction is 1 (x 1: the output to the bit). Not
 * made up: the oscillators' +0.55 dB with 6 voices on or more (VC6_GAIN), which fewer voices may lose */
SY_CTLI static float pan_l(float p);
SY_CTLI static float pan_r(float p);
static SY_NSL float uni_pw(const sy_synth *s, int n, int i)   /* a unison slot's power (voice_block's level and pan) */
{
    float x = uni_x(n, i), p = sy_clampf(s->pv[PAN] + s->pv[UNI_SPREAD] * (128.f / 127.f) * x, 0.f, 127.f);
    float l = pan_l(p), r = pan_r(p);
    return (n < 2 ? 1.f : sy_exp2((UNI_DB + UNI_TAPER * (1.f - sy_absf(x))) * 0.332192809f)) * (l * l + r * r);
}
static SY_NSL int uni_fade_b(const sy_synth *s)   /* the fade's blocks: ~15 ms (5 at 44.1 kHz) */
{
    int b = (int)(0.015f / s->bdt + 0.5f);
    return b < 1 ? 1 : b > 255 ? 255 : b;
}
static SY_NSL void uni_level(sy_synth *s, int key, int np, int nb)   /* the note on key (its patch's count np):
                                         * its voices' correction, ramped in nb blocks (0: at once) */
{
    float pr = 0.f, pn = 0.f, g;
    int i, k;
    for (i = 0; i < np; i++) pr += uni_pw(s, np, i);
    for (i = 0; i < SY_NV(s); i++) {
        const sy_voice_t *v = &s->v[i];
        if (v->on && v->gate && v->key == key && v->uni_p == np) pn += uni_pw(s, v->uni_n, v->uni_i);
    }
    for (k = 0; k < s->upn; k++)
        if (s->upend[k].key == key)
            for (i = 0; i < 16; i++) if (s->upend[k].mask >> i & 1u) pn += uni_pw(s, s->upend[k].n, i);
    g = pn > 0.f && sy_absf(pr - pn) > 1e-4f * pr ? SY_FAR(sy_sqrt)(pr / pn) : 1.f;
    for (i = 0; i < SY_NV(s); i++) {
        sy_voice_t *v = &s->v[i];
        if (!(v->on && v->gate && v->key == key && v->uni_p == np)) continue;
        v->ugt = g;
        v->ufb = (uint8_t)nb;
        if (!nb) v->ug = g;
    }
}
/* a voice taken from a sounding note fades out in nb blocks (straight in power; voice_block ends it), its note's other
 * voices taking over its level in the same time (comp: a held note's unison voice); the new note takes another voice
 * meanwhile, over the cap (uni_spare), or waits for this one (uni_pend) */
#ifndef UNI_FADE_S
#define UNI_FADE_S 1                    /* (after 1.18C32) 1: a voice taken fades out on a smooth curve in level (the
                                         * level r^2 (3 - 2 r) of the fade's rest r, its slope 0 at both ends: no step in
                                         * the slope at the end as the straight power fade's sqrt had, which clicked on a
                                         * dark, loud voice), the guard's and a steal's alike; 0: straight in power */
#endif
#if UNI_FADE_S
/* the fading voice's level a block on (UNI_FADE_S): ufade holds the fade's length in blocks, ufb the blocks left; the
 * level scaled by the curve's step (in the flash: per block of a fade only) */
static SY_NSL float uni_fade_s(const sy_voice_t *v)
{
    const float n = (float)v->ufade, r0 = (float)v->ufb / n, r1 = (float)(v->ufb - 1) / n;
    const float c0 = r0 * r0 * (3.f - 2.f * r0), c1 = r1 * r1 * (3.f - 2.f * r1);
    return c0 > 0.f ? v->ug * (c1 / c0) : 0.f;
}
#endif
static SY_NSL void uni_fade(sy_synth *s, sy_voice_t *w, int nb, int comp)
{
    int held = w->gate;
    w->gate = 0;
    w->ufade = (uint8_t)(UNI_FADE_S ? (nb < 1 ? 1 : nb > 255 ? 255 : nb) : 1);   /* (UNI_FADE_S: the fade's length) */
    w->ugt = 0.f;
    w->ufb = (uint8_t)nb;
    w->rph1 = w->ph1; w->rph2 = w->ph2; w->rph3 = w->ph3; w->rphs = w->phs;   /* (as a release: REL_PH) */
    if (held && comp) uni_level(s, w->key, w->uni_p, nb);
}
#ifndef UNI_OVER
#define UNI_OVER 2                      /* the voices fading out over the cap at most (the load: ~15 ms of them) */
#endif
/* (1.18C49, a firmware's budget: app.c GOV_BUD; not the reference's) sy_bud set: the part's voices stay
 * within the cap, those fading counted: no voice over it while another fades out (UNI_OVER 0), and a voice waiting
 * for a fade (uni_pend) starts only as that fade has ended. A chord change of a patch that overloads the block then
 * never runs more voices than its budget allows (the new note starts as the old fades end, ~15 ms later, as
 * REL_FADE's). 0 (as shipped by the engine; the firmware sets it only while its budget acts): as 1.18C48 */
volatile uint8_t sy_bud;
static SY_NSL sy_voice_t *uni_spare(sy_synth *s, int over)   /* a free voice of the part (over: one that takes the
                                         * place of a voice fading out, UNI_OVER over the cap at most; with sy_bud
                                         * none over it), 0 none */
{
    int i, on = 0, lim = s->max_voices;
    if (over) {
        for (i = 0; i < SY_NV(s); i++) on += s->v[i].on;
#ifdef HYB_MULTI
        if (mx_m && mx_m->dyn_now && mx_m->mode_now != SY_M_SINGLE) {   /* (DYNAMIC: the pool's cap) */
            lim = mx_m->cap_now;
            on += mx_on_n(mx_m->p[s->pidx ^ 1]);
        }
#endif
        if (on >= lim + (sy_bud ? 0 : UNI_OVER)) return 0;
    }
    for (i = 0; i < SY_NV(s); i++) if (!s->v[i].on) return &s->v[i];
#ifdef HYB_MULTI
    if (mx_m && mx_m->dyn_now && mx_m->mode_now != SY_M_SINGLE) {   /* (DYNAMIC: a free one of the other part) */
        sy_synth *o = mx_m->p[s->pidx ^ 1];
        if (mx_on_n(o) < o->nv) {
            mx_move(s);
            for (i = 0; i < SY_NV(s); i++) if (!s->v[i].on) return &s->v[i];
        }
    }
#endif
    return 0;
}
/* UNISON FIRST's allocation (alloc_voice's rules; the voices fading not counted, those waiting counted): a free voice
 * under the cap; else the oldest released voice; else a held note's extra unison voice (uni_victim); else the oldest
 * held voice. A voice taken fades out (~15 ms) while a free voice over the cap (UNI_OVER) takes the note at once.
 * Without one a released or a whole note's voice is cut, as alloc_voice does; a unison voice fades in one block and
 * the new voice waits for it (0 returned: uni_wait, uni_pend): the note a block late (2.9 ms), complete. how: 0 the
 * note's first voice, 1 another (UNI_WAIT_FAST 0: it waits for a full fade, the note sounding meanwhile with the
 * voices it has), 2 a free voice only (uni_pend) */
#ifndef UNI_WAIT_FAST
#define UNI_WAIT_FAST 1
#endif
static SY_NSL sy_voice_t *uni_alloc(sy_synth *s, int how)
{
    sy_voice_t *f;
    int i, best = -1, live = 0;
    float bl = 1e9f;
    uint32_t oldest = 0xFFFFFFFFu;
#ifdef HYB_MULTI
    int room;
#endif
    if (how >= 2) return uni_spare(s, how == 3);   /* (3: sy_bud's, under the cap: uni_pend) */
#ifdef HYB_MULTI
    room = mx_room(s);
#else
#define room 1
#endif
    for (i = 0; i < SY_NV(s); i++) live += s->v[i].on && !s->v[i].ufade;
    for (i = 0; i < 16 * s->upn; i++) live += s->upend[i >> 4].mask >> (i & 15) & 1u;   /* (and those waiting) */
    if ((!POLY_LOAD || live < patch_poly(s)) && live < s->max_voices && room && (f = uni_spare(s, 0)) != 0) return f;
#undef room
    for (i = 0; i < SY_NV(s); i++)
        if (s->v[i].on && !s->v[i].gate && !s->v[i].ufade && (STEAL_LVL ? s->v[i].aenv.lvl < bl : (float)s->v[i].age < bl)) {
            bl = STEAL_LVL ? s->v[i].aenv.lvl : (float)s->v[i].age; best = i;
        }
    if (best >= 0) {                    /* (a released voice) */
        if ((f = uni_spare(s, 1)) != 0) { uni_fade(s, &s->v[best], uni_fade_b(s), 0); return f; }
#if REL_FADE
        if (s->v[best].aenv.lvl > REL_FADE_LVL) { uni_fade(s, &s->v[best], REL_FADE_NB(s), 0); return 0; }   /* (1.18C48) */
#endif
        return &s->v[best];
    }
    best = uni_victim(s);
    if (best >= 0) {                    /* (a held note's unison voice) */
        f = uni_spare(s, 1);
        uni_fade(s, &s->v[best], f || (how && !UNI_WAIT_FAST) ? uni_fade_b(s) : 1, 1);
        return f;
    }
    for (i = 0; i < SY_NV(s); i++)      /* (the oldest held voice: a whole note) */
        if (s->v[i].on && !s->v[i].ufade && s->v[i].age < oldest) { oldest = s->v[i].age; best = i; }
    if (best < 0) return uni_spare(s, 0);
    if ((f = uni_spare(s, 1)) != 0) { uni_fade(s, &s->v[best], uni_fade_b(s), 0); return f; }
    if (STEAL_KEEP && s->steal_n < 16) SY_STEAL_PUT(&s->steal[s->steal_n++], &s->v[best]);
    return &s->v[best];
}
#endif

static SY_NSL void voice_start(sy_synth *s, sy_voice_t *v, int key, int vel, int ui, int un, float from, int keep)
{
    int i, reset, others = 0;
    for (i = 0; i < s->max_voices; i++) if (&s->v[i] != v && s->v[i].on && s->v[i].key != key) others = 1;
    v->on = 1;
    v->gate = 1;
    v->key = (uint8_t)key;
    v->vel = (uint8_t)vel;
    v->iph = s->patch[B_INIT_PH];       /* (the matrix's offset: voice_block's first block) */
    reset = v->iph != 0;
    v->uni_i = (uint8_t)ui;
    v->uni_n = (uint8_t)un;
    v->age = ++s->ages;
    v->fresh = 1;
#ifdef SY_UNI_CAP
    v->uni_p = (uint8_t)un;             /* (note_on sets the patch's count; the correction 1 until uni_level) */
    v->ug = v->ugt = 1.f;
    v->ufade = v->ufb = 0;
#endif
    v->target = (float)key;
    v->glide = from;
    v->t_on = 0.f;
    v->started = 0;
    if (!(keep && MONO_KEEP_CLK)) {
        v->click = 0.f;                 /* (the transient's position in its table) */
        v->click_lp = 0.f;
    }
    if (reset || !v->aenv.stage) {      /* osc init phase 1-127: a reset at every note on (0: free running) */
        if (reset) {   /* the value is a start phase: osc 2 at +v/128 cycle, osc 3 at -v/128 (mirrored) */
            uint32_t q = (uint32_t)v->iph << 25;
            v->ph1 = v->phs = 0;
            v->ph2 = q;
            v->ph3 = 0u - q;
        }
        v->lp_osc = v->lp_sub = v->lp_x = v->lp_osc2 = v->lp_x2 = 0.f;
    }
    if (!(keep && MONO_KEEP_FILT)) {
        v->f1.s1 = v->f1.s2 = v->f2a.s1 = v->f2a.s2 = v->f2b.s1 = v->f2b.s2 = 0.f;
        for (i = 0; i < 4; i++) v->lad[i] = 0.f;
        v->lad_a = 0.f;
        v->lad[0] = 1e-5f;              /* a seed: a self-oscillating analog mode starts by itself (filt2.json ring) */
    }
    if (!(keep && MONO_KEEP_SAT)) {
        v->sat_hold = v->sat_lp = v->sat_dc = 0.f;
        v->sat_cnt = -1.f;             /* (the sample reducers: take one at the first frame) */
    }
    if (!keep) v->aenv.lvl = v->aenv.prev = 0.f;
    if (!(keep && MONO_KEEP_FENV)) v->fenv.lvl = v->fenv.prev = 0.f;
    env_start(&v->aenv);
    env_start(&v->fenv);
    for (i = 0; i < 3; i++) {
        static const int TRIG[3] = {L1_TRIG, L2_TRIG, -1};
        int trig = i < 2 ? s->patch[TRIG[i]] : 0;
        v->lfo[i] = s->glfo[i];         /* free running: continue from the part's phase */
        (void)others;                   /* (a voice added to sounding ones: its free LFOs' phase at its first block,
                                         * CLN_LFO_TAB) */
        v->lfo[i].rng = v->rng + (uint32_t)i * 977u;
        if (trig) lfo_trigger(&v->lfo[i]);
        v->lfo[i].done = 0;
        if ((i == 0 && s->patch[L1_ENV]) || (i == 1 && s->patch[L2_ENV])) lfo_trigger(&v->lfo[i]);
    }
#ifdef CLN_LFO_TAB
    v->lfo_pend = (uint8_t)!keep;       /* (decided at the voice's first block, see sy_render) */
#endif
}

static int mono_mode(const sy_synth *s) { int m = s->patch[KEY_MODE]; return m >= 1 && m <= 4; }
#ifdef SY_UNI_CAP
/* the unison caps (SY_UNI_CAP; the player's, not the patch's): a note's unison count n (the patch's) at its start is
 * at most uni_mono (a mono patch) or uni_max (0: no cap). uni_auto (poly): the cap steps down as the part's chord
 * grows, the keys held with this one 1-2: uni_max (or the patch's), 3-4: 2, 5 and more: 1, and never more than the
 * voices still free of held notes (the cap less the voices whose key is held), at least 1: a big chord takes its
 * notes without stealing held ones. A note with fewer voices spreads them as the engine spreads that count (uni_x:
 * 2 voices at +-the full detune, the width kept). Mono patches: uni_mono alone. */
static SY_NSL int uni_cap(const sy_synth *s, int n, int key)
{
    int lim;
    if (mono_mode(s)) lim = s->uni_mono;
    else {
        lim = s->uni_max;
        if (s->uni_auto) {
            int k, held = 1, gated = 0, cap = s->max_voices, room;
            for (k = 0; k < 128; k++) held += s->held[k] && k != key;
            for (k = 0; k < SY_NV(s); k++) gated += s->v[k].on && s->v[k].gate;
#ifdef HYB_MULTI
            if (mx_m && mx_m->dyn_now && mx_m->mode_now != SY_M_SINGLE) {   /* (DYNAMIC: the pool's) */
                const sy_synth *o = mx_m->p[s->pidx ^ 1];
                cap = mx_m->cap_now;
                for (k = 0; k < o->nv; k++) gated += o->v[k].on && o->v[k].gate;
            }
#endif
            if (held >= 5) lim = 1;
            else if (held >= 3 && (!lim || lim > 2)) lim = 2;
            room = cap - gated;
            if (room < 1) room = 1;
            if (!lim || lim > room) lim = room;
        }
    }
    return lim && n > lim ? lim : n;
}
#endif

/* a note's unison voice i of n: a voice (alloc_voice; with UNISON FIRST uni_alloc, how as there: 0 none, the voice
 * waits), started, its oscillators' phases handed over (note_on), its tuning step */
static SY_NSL sy_voice_t *note_voice(sy_synth *s, int key, int vel, int i, int n, float from, uint32_t n0, int alone, int how)
{
    sy_voice_t *src = 0, *v;
    uint32_t best = 0, p1 = 0, p2 = 0, p3 = 0, ps = 0;
    int j;
    if (alone)
        for (j = 0; j < s->max_voices; j++)
            if (s->v[j].used && s->v[j].solo && s->v[j].uni_i == i && s->v[j].uni_n == n && s->v[j].age >= best) { src = &s->v[j]; best = s->v[j].age; }
    if (!src)
        for (j = 0; j < s->max_voices; j++)
            if (s->v[j].used && s->v[j].key == key && s->v[j].uni_i == i && s->v[j].uni_n == n && s->v[j].age >= best) { src = &s->v[j]; best = s->v[j].age; }
    if (src) {
        if (REL_PH && !src->gate) { p1 = src->rph1; p2 = src->rph2; p3 = src->rph3; ps = src->rphs; }
        else { p1 = src->ph1; p2 = src->ph2; p3 = src->ph3; ps = src->phs; }
    }
#ifdef SY_UNI_CAP
    if (how >= 0) {
        v = uni_alloc(s, how);
        if (!v) return 0;
    } else
#endif
    v = alloc_voice(s);
#if REL_FADE && defined(SY_UNI_CAP)
    if (!v) return 0;                   /* (1.18C48: a release tail fading for it: uni_wait) */
#endif
    (void)how;
    voice_start(s, v, key, vel, i, n, from, 0);
    if (PHASE_TAB) {
        ptab_take(s, v);
        v->phs = (v->ph1 >> 1) + SUB_C;   /* the sub is not handed over with the phases: it starts in its upper
                                         * half at every note, then flips each time osc 1 passes the half of its cycle
                                         * (read on the sub's start phase, fresh and handed-over voices alike) */
#if IPH_RESET
        if (v->iph) {                   /* CLEANER: the osc init phase (B 35 = 1-127) resets the phases at every note
                                         * on, the hand-over's too (measured, black box: the reference's note-on phase
                                         * is the same at every note, a note's unison voices in phase; the table took
                                         * it away: bass/iph.py, PROVENANCE 11). The table's bookkeeping stays */
            const uint32_t q = (uint32_t)v->iph << 25;
            v->ph1 = v->phs = 0; v->ph2 = q; v->ph3 = 0u - q;
        }
#endif
    }
    else if (src) { v->ph1 = p1; v->ph2 = p2; v->ph3 = p3; v->phs = ps; v->used = 1; }
    else v->used = 0;
    v->solo = (uint8_t)alone;
    v->du = TUNE_STEP[(n0 + (UNI_PAIRS ? (uint32_t)uni_step(n, i) : 0u)) & 15u];
    v->lfo_step = n0 + (UNI_PAIRS ? (uint32_t)uni_step(n, i) : 0u);
    return v;
}
#ifdef SY_UNI_CAP
/* UNISON FIRST, the pool full: a note's unison voice waits for the voice it took to fade out (uni_alloc), its
 * level counted as playing (uni_level); the four latest notes' (a fifth's voice is left out, its note made as loud
 * with the others) */
static SY_NSL void uni_wait(sy_synth *s, int key, int vel, int i, int n, int np, int alone, uint32_t n0, float from)
{
    int k;
    for (k = 0; k < s->upn; k++) if (s->upend[k].key == key && s->upend[k].n0 == n0) break;
    if (k == s->upn) {
        if (k == 4) return;
        s->upn++;
        s->upend[k].key = (uint8_t)key; s->upend[k].vel = (uint8_t)vel; s->upend[k].n = (uint8_t)n;
        s->upend[k].np = (uint8_t)np; s->upend[k].alone = (uint8_t)alone; s->upend[k].wait = 0;
        s->upend[k].mask = 0; s->upend[k].n0 = n0; s->upend[k].from = from;
    }
    s->upend[k].mask |= (uint16_t)(1u << i);
}
/* a block's start (before its events): the voices waiting take the voices the fades freed; a note released meanwhile,
 * or not served in twice the fade's time, gives its waiting voices up (its sounding ones made as loud) */
static SY_NSL void uni_pend(sy_synth *s)
{
    int k = 0, i;
    while (k < s->upn) {
        int key = s->upend[k].key, np = s->upend[k].np, go = 0, j;
        if ((s->held[key] || s->patch[KEY_MODE] == 5) && s->upend[k].wait++ < 2 * uni_fade_b(s)) {
            for (i = 0; i < 16 && s->upend[k].mask; i++)
                if (s->upend[k].mask >> i & 1u) {
                    sy_voice_t *v = note_voice(s, key, s->upend[k].vel, i, s->upend[k].n, s->upend[k].from,
                                               s->upend[k].n0, s->upend[k].alone,
                                               sy_bud && s->upend[k].wait <= uni_fade_b(s) + 1 ? 3 : 2);   /* (sy_bud:
                                         * under the cap for a fade's time and a block, then any free voice as
                                         * before: the note never left out) */
                    if (!v) break;
                    v->uni_p = (uint8_t)np;
                    s->upend[k].mask &= (uint16_t)~(1u << i);
                    go = 1;
                }
            if (s->upend[k].mask) {
                if (go) uni_level(s, key, np, 0);
                k++;
                continue;
            }
        } else go = -1;
        for (j = k + 1; j < s->upn; j++) s->upend[j - 1] = s->upend[j];
        s->upn--;
        uni_level(s, key, np, go < 0 ? uni_fade_b(s) : 0);
    }
}

/* Not the reference's behaviour (an own choice, 1.09): UNISON turned while notes are held changes them at once (UNI_LIVE). The
 * reference takes a new count at the next note-on only (MEASURED: a held note, the count changed 1 -> 7 and 7 -> 1
 * by its controller: the note's level and beating as before, to the sample statistics; the next note has the new
 * count). Each held note takes the count a note-on would give it now (the patch's, the caps: uni_cap, the voices
 * there are); its voices keep their order and take the new count's slots (detune, pan, tuning step: the oscillators
 * and envelopes run on); voices added are copies of the note's first voice, faded in (~15 ms, uni_level), taken
 * from the free voices under the cap, else the oldest released ones (never a held note's: a note that finds none
 * keeps fewer, as loud: uni_level); voices dropped fade out as UNISON FIRST's (uni_fade). Notes with voices waiting
 * (uni_pend) are left as they are. No change without a change of the count: the null tests unchanged */
static SY_NSL sy_voice_t *uni_more(sy_synth *s)   /* a voice for an added slot (0: none) */
{
    sy_voice_t *f;
    int i, best = -1, live = 0, lim = s->max_voices;
    uint32_t old = 0xFFFFFFFFu;
    for (i = 0; i < SY_NV(s); i++) live += s->v[i].on && !s->v[i].ufade;
#ifdef HYB_MULTI
    if (mx_m && mx_m->dyn_now && mx_m->mode_now != SY_M_SINGLE) {   /* (DYNAMIC: the pool's cap) */
        lim = mx_m->cap_now;
        live += mx_on_n(mx_m->p[s->pidx ^ 1]);
    }
#endif
    if (live < lim && (!POLY_LOAD || live < patch_poly(s)) && (f = uni_spare(s, 0)) != 0) return f;
    for (i = 0; i < SY_NV(s); i++)
        if (s->v[i].on && !s->v[i].gate && !s->v[i].ufade && s->v[i].age < old) { old = s->v[i].age; best = i; }
    if (best < 0) return 0;
    if ((f = uni_spare(s, 1)) != 0) { uni_fade(s, &s->v[best], uni_fade_b(s), 0); return f; }
    return &s->v[best];
}
static SY_COLD void uni_live(sy_synth *s)
{
    int key, i, j, k, nb = uni_fade_b(s);
    for (key = 0; key < 128; key++) {
        sy_voice_t *w[16], *t;
        int c = 0, np = s->patch[UNISON] + 1, n;
        uint32_t n0;
        for (i = 0; i < SY_NV(s) && c < 16; i++)
            if (s->v[i].on && s->v[i].gate && !s->v[i].ufade && s->v[i].key == key) w[c++] = &s->v[i];
        if (!c) continue;
        for (k = 0; k < s->upn && s->upend[k].key != key; k++)
            ;
        if (k < s->upn) continue;       /* (voices waiting: as they are) */
        n = uni_cap(s, np, key);
#ifdef HYB_MULTI
        if (n > MX_ULIM(s)) n = MX_ULIM(s);
        if (np > MX_ULIM(s)) np = MX_ULIM(s);
#else
        if (n > s->max_voices) n = s->max_voices;
        if (np > s->max_voices) np = s->max_voices;
#endif
        if (n < 1) n = 1;
        for (i = 1; i < c; i++)         /* (in their slots' order) */
            for (j = i; j > 0 && w[j - 1]->uni_i > w[j]->uni_i; j--) { t = w[j]; w[j] = w[j - 1]; w[j - 1] = t; }
        for (i = 0; i < c && w[i]->uni_n == n && w[i]->uni_i == i && w[i]->uni_p == np; i++)
            ;
        if (i == c && c == n) continue; /* (as it is) */
        n0 = w[0]->lfo_step - (UNI_PAIRS ? (uint32_t)uni_step(w[0]->uni_n, w[0]->uni_i) : 0u);
        for (i = n; i < c; i++) uni_fade(s, w[i], nb, 0);   /* (the slots dropped) */
        for (i = c; i < n && i < 16; i++) {   /* (the slots added) */
            sy_voice_t *f = uni_more(s);
            uint32_t rng;
            if (!f) break;
            rng = f->rng;
            *f = *w[0];
            f->rng = rng;               /* (its own noise) */
            f->ph1 += (uint32_t)i * 0x9E3779B9u;   /* (its own phases: a copy in phase would add up coherently) */
            f->phs += (uint32_t)i * 0x4F1BBCDCu;   /* (the sub: half osc 1's shift, its cycle being twice as long) */
            f->ph2 += (uint32_t)i * 0x7F4A7C15u;
            f->ph3 += (uint32_t)i * 0x2545F491u;
            f->age = ++s->ages;
            f->ug = f->ugt = 0.f;       /* (from silence: uni_level ramps it in) */
            f->ufb = 0;
            f->gl = f->gr = 0.f;
            w[c++] = f;
        }
        if (c > n) c = n;
        for (i = 0; i < c; i++) {       /* (the new count's slots) */
            w[i]->uni_i = (uint8_t)i;
            w[i]->uni_n = (uint8_t)c;
            w[i]->uni_p = (uint8_t)np;
            w[i]->du = TUNE_STEP[(n0 + (UNI_PAIRS ? (uint32_t)uni_step(c, i) : 0u)) & 15u];
            w[i]->lfo_step = n0 + (UNI_PAIRS ? (uint32_t)uni_step(c, i) : 0u);
        }
        uni_level(s, key, np, nb);
    }
}
#endif

SY_COLD static void note_on(sy_synth *s, int key, int vel)
{
    int n = s->patch[UNISON] + 1, i, porta = s->patch[PORTA] > 0;
    float from = porta ? s->last_note : (float)key;
#ifdef SY_UNI_CAP
    int np = n, ns = 0;                 /* (the patch's count; the voices started) */
#endif
    if (s->last_note == 0.f) from = porta ? 60.f : (float)key;     /* the first glide starts at key 60 */
#ifdef SY_UNI_CAP
    n = uni_cap(s, n, key);
#endif
#ifdef HYB_MULTI
    if (n > MX_ULIM(s)) n = MX_ULIM(s);
#ifdef SY_UNI_CAP
    if (np > MX_ULIM(s)) np = MX_ULIM(s);
#endif
#else
    if (n > s->max_voices) n = s->max_voices;
#ifdef SY_UNI_CAP
    if (np > s->max_voices) np = s->max_voices;
#endif
#endif
    s->held[key] = 1;
    if (mono_mode(s)) {
        int legato = 0, mono_new = 0;
        if (s->nstack < 16) {
            for (i = 0; i < s->nstack; i++) if (s->stack[i] == key) break;
            if (i == s->nstack) { s->stack[s->nstack++] = (uint8_t)key; s->kcount[key] = 1; }
            else if (EV_ONS_FIRST) {    /* the key is down already (its off comes later in this block): it
                                         * only counts (read: mono, off + on of one key in a block: the note goes on) */
                if (s->kcount[key] < 255) s->kcount[key]++;
                if (LEGATO_RETRIG && s->patch[KEY_MODE] <= 2)   /* (modes 1-2 restart the envelopes, as legato) */
                    for (i = 0; i < s->max_voices; i++)
                        if (s->v[i].on && s->v[i].gate) { env_start(&s->v[i].aenv); env_start(&s->v[i].fenv); }
                return;
            }
        }
        for (i = 0; i < s->max_voices; i++)
            if (s->v[i].on && s->v[i].gate) {   /* legato: the pitch moves, the envelopes go on */
                s->v[i].target = (float)key;
                s->v[i].key = (uint8_t)key;
                if (!porta) s->v[i].glide = (float)key;
                if (LEGATO_RETRIG && s->patch[KEY_MODE] <= 2) {   /* key modes 1-2 restart both envelopes
                                         * from where they are (read: an overlapping note at attack 0, decay 70,
                                         * sustain 100: back to the peak; modes 3-4 go on) */
                    env_start(&s->v[i].aenv);
                    env_start(&s->v[i].fenv);
                    if (LEG_FRESET) {   /* (test: the filter states afresh on a legato note) */
                        sy_voice_t *w = &s->v[i]; int j;
                        w->f1.s1 = w->f1.s2 = w->f2a.s1 = w->f2a.s2 = w->f2b.s1 = w->f2b.s2 = 0.f;
                        for (j = 0; j < 4; j++) w->lad[j] = 0.f;
                        w->lad[0] = 1e-5f;
                    }
                }
                legato = 1;
            }
        if (legato) { s->last_note = (float)key; return; }
        if (MONO_KEEP_LVL > 0.f) {      /* a release already below MONO_KEEP_LVL: a new voice (see there) */
            float g = sy_db2lin(-60.f / sy_tab(s, T_T60, s->pv[AE_R]) * MONO_KEEP_LAT * s->bdt);
            for (i = 0; i < s->max_voices; i++)
                if (s->v[i].on && s->v[i].aenv.stage == 4 && s->v[i].aenv.lvl * g < MONO_KEEP_LVL) mono_new = 1;
        }
        if (!mono_new) {   /* a note after the release goes on with the same voices: the envelopes attack from where the
             * release has got to, the oscillators run on (read: key modes 1 and 2, a sine, the next note 5-60 blocks
             * after the release, attack 0 and 60: no dip; a release that has ended starts from 0) */
            int m = 0;
            for (i = 0; i < s->max_voices; i++) m += s->v[i].on && s->v[i].uni_n == n;
            if (m == n) {               /* (the tuning step stays: read, 12 such notes) */
                for (i = 0; i < s->max_voices; i++)
                    if (s->v[i].on) voice_start(s, &s->v[i], key, vel, s->v[i].uni_i, n, from, 1);
#ifdef SY_UNI_CAP
                if (np != n) {
                    for (i = 0; i < s->max_voices; i++) if (s->v[i].on) s->v[i].uni_p = (uint8_t)np;
                    uni_level(s, key, np, 0);
                }
#endif
                s->last_note = (float)key;
                return;
            }
        }
        for (i = 0; i < s->max_voices; i++)
            if (s->v[i].on) {           /* (a new voice after a low release: the old one fades out over its next block,
                                         * the amp's line to 0 as at a release's end; voice_block ends it) */
                if (mono_new && s->v[i].aenv.stage == 4) s->v[i].aenv.stage = 0;
                else { s->v[i].on = 0; s->v[i].aenv.stage = 0; }
            }
    }
    {
        /* free-running voices (read: osc 1 + 2 detuned, the pair's phase per note; sines, a key struck again
         * 1-40 blocks after its release, the new note's phase against the old one's). A new note always takes a
         * fresh voice: the old voice's release sounds on under it (read: the tail adds to the new note at full
         * level, both keys and chords). Its oscillators start at the phases the last voice of the same key and
         * unison slot had at its release (they stood since: the same phase whatever the gap, both oscillators,
         * lone notes and chord tones alike). A key without such a voice, struck while no other key is held, takes
         * over the phases of the last such lone note's voice, whatever its key; a chord tone without one starts
         * its oscillator pair afresh. */
        int alone = 1, j;
        uint32_t n0 = s->notes;
        s->notes += UNI_PAIRS ? (uint32_t)(n + 1) / 2u : 1u;
        for (j = 0; j < s->max_voices; j++) if (s->v[j].on && s->v[j].gate && s->v[j].key != key) alone = 0;
        for (i = 0; i < n; i++) {
#ifdef SY_UNI_CAP
            sy_voice_t *v = note_voice(s, key, vel, i, n, from, n0, alone, s->uni_steal ? (ns ? 1 : 0) : -1);
            if (!v) { uni_wait(s, key, vel, i, n, np, alone, n0, from); continue; }
            v->uni_p = (uint8_t)np;
            ns++;
#else
            note_voice(s, key, vel, i, n, from, n0, alone, -1);
#endif
        }
        for (j = 0; j < s->steal_n; j++) ptab_keep_st(s, &s->steal[j]);
        s->steal_n = 0;
#ifdef SY_UNI_CAP
        if (np != n || ns < n) uni_level(s, key, np, 0);   /* (fewer voices than the patch's, or one left out by
                                         * uni_wait: as loud) */
#endif
    }
    s->last_note = (float)key;
}

SY_COLD static void voice_release(sy_synth *s, sy_voice_t *v)
{
    v->gate = 0;
    v->rph1 = v->ph1; v->rph2 = v->ph2; v->rph3 = v->ph3; v->rphs = v->phs;   /* (the phases at the release: REL_PH) */
    if (PHASE_TAB) ptab_keep(s, v);
    if (v->aenv.stage) v->aenv.stage = 4;
    if (v->fenv.stage) v->fenv.stage = 4;
}

SY_COLD static void note_off(sy_synth *s, int key)
{
    int i;
    s->held[key] = 0;
    if (!mono_mode(s) && s->nstack) {   /* a key struck in a mono mode and let go after the key mode turned
                                         * poly (or hold) leaves the mono stack too. Before, its stale entry made the
                                         * key dead once the patch was mono again (note_on took it for a key already
                                         * down: no note) until all notes off; the stack full of such keys, a mono
                                         * note's voices were never released. (A poly patch's stack is empty: the
                                         * null tests unchanged) */
        int j, k = 0;
        for (j = 0; j < s->nstack; j++) if (s->stack[j] != key) s->stack[k++] = s->stack[j];
        s->nstack = (uint8_t)k;
        s->kcount[key] = 0;
    }
    if (s->patch[KEY_MODE] == 5) return;    /* hold */
    if (mono_mode(s)) {
        int j, k = 0;
        if (EV_ONS_FIRST && s->kcount[key] > 1) { s->kcount[key]--; s->held[key] = 1; return; }
        s->kcount[key] = 0;
        for (j = 0; j < s->nstack; j++) if (s->stack[j] != key) s->stack[k++] = s->stack[j];
        s->nstack = (uint8_t)k;
        if (k) {                        /* back to the last held note (last-note priority) */
            int nk = s->stack[k - 1];
            for (i = 0; i < s->max_voices; i++)
                if (s->v[i].on && s->v[i].gate) {
                    if (LEGATO_RETRIG && s->patch[KEY_MODE] <= 2 && s->v[i].key == key && nk != key) {
                        env_start(&s->v[i].aenv);   /* modes 1-2, back to a held key: the envelopes
                                         * restart from where they are, as on a legato note (read) */
                        env_start(&s->v[i].fenv);
                    }
                    s->v[i].target = (float)nk;
                    s->v[i].key = (uint8_t)nk;
                    if (!s->patch[PORTA]) s->v[i].glide = (float)nk;
                }
            s->last_note = (float)nk;
            return;
        }
    }
    {
        int older = 0;
        uint32_t mark = s->age_mark;
        if (EV_ONS_FIRST)
            for (i = 0; i < SY_NV(s); i++)
                if (s->v[i].on && s->v[i].gate && s->v[i].key == key && s->v[i].age <= s->age_mark) older = 1;
        if (EV_ONS_FIRST && off_pre) {  /* CLEANER (1.18C34 fix): an off queued before its key's note-on in this block
                                         * is the notes' before it: their voices alone end; none left (the note-on took
                                         * the part's last voice, the old note's: a part with one voice, an arp pattern
                                         * with every step on) nothing, where up to 1.18C34 it ended the new note */
            older = 1;
            mark = off_lim;
        }
        for (;;) {                      /* the voices release oldest first, a note's unison voices in their
                                         * order (read on the order their phases are handed over) */
            int b = -1;
            for (i = 0; i < SY_NV(s); i++) {
                const sy_voice_t *v = &s->v[i];
                if (v->on && v->gate && v->key == key && (!older || v->age <= mark) &&
                    (b < 0 || v->age < s->v[b].age || (v->age == s->v[b].age && v->uni_i < s->v[b].uni_i))) b = i;
            }
            if (b < 0) break;
            voice_release(s, &s->v[b]);
        }
    }
#if SLIDE_SOLO
    {   /* a note struck in the same block as the last other key's release (a slide) counts as struck alone:
         * it takes over the last lone note's oscillator phases (read: the test bank's sequence, its held note's
         * pair level per note-on time) */
        int j, others = 0;
        for (j = 0; j < s->max_voices; j++) if (s->v[j].on && s->v[j].gate && s->v[j].started) others = 1;
        if (!others)
            for (i = 0; i < s->max_voices; i++) {
                sy_voice_t *v = &s->v[i], *src = 0;
                uint32_t best = 0;
                if (!(v->on && v->gate && !v->started && !v->solo)) continue;
                for (j = 0; j < s->max_voices; j++)
                    if (&s->v[j] != v && s->v[j].used && s->v[j].solo && s->v[j].uni_i == v->uni_i && s->v[j].uni_n == v->uni_n && s->v[j].age >= best) { src = &s->v[j]; best = s->v[j].age; }
                if (src) { v->ph1 = src->ph1; v->ph2 = src->ph2; v->ph3 = src->ph3; v->phs = src->phs; v->used = 1; }
                v->solo = 1;
            }
    }
#endif
}

void sy_all_off(sy_synth *s)
{
    int i;
    for (i = 0; i < SY_NV(s); i++) if (s->v[i].on) voice_release(s, &s->v[i]);
    for (i = 0; i < 128; i++) s->held[i] = s->arp_vel[i] = s->arp_chord_k[i] = 0;
#ifdef SY_UNI_CAP
    s->upn = 0;
#endif
    arp_held = 0;
    s->nstack = 0;
    s->arp_n = 0;
}

static void arp_note_on(sy_synth *s, int key, int vel);
/* the tempo: an external MIDI clock's while it runs (the ticks' smoothed spacing: the clocked LFOs and
 * the delay), else the patch's (B 16 + 63) */
/* the voices' control blocks in RAM (SY_CTL_RAM, a firmware's choice: on the FM-1 a cold line of the flash costs ~500
 * cycles, and these run once a block between the main loop's own code) */
#ifdef SY_CTL_RAM
#define SY_CTL __attribute__((section(".ram_text")))
#else
#define SY_CTL
#endif
static float tempo_bpm(const sy_synth *s)
{
    if (s->xclk_on && s->xclk_per > 0.f) return 60.f * s->fs / (24.f * s->xclk_per);
    return (float)s->patch[B_TEMPO] + 63.f;
}
/* the arpeggiator's tempo under a MIDI clock. Read: its steps run on its own timer from the key (the first
 * note at the key, the steps not on the clock's grid). After a start its tempo glides in: from the first ticks'
 * spacing it starts at the mean of the patch's tempo and the clock's, drifting to the clock's (3 % per tick); after
 * a beat it takes the clock's, and later changes at once. (Not modelled: at that switch the reference plays one
 * longer step, 1.7-3.4 steps.) */
#ifndef XCLK_SETTLE
#define XCLK_SETTLE 24u
#endif
#ifndef XCLK_GLIDE
#define XCLK_GLIDE 0.03f
#endif
static float arp_bpm(const sy_synth *s)
{
    if (s->xclk_on && s->xclk_arp > 0.f) return 60.f * s->fs / (24.f * s->xclk_arp);
    return tempo_bpm(s);
}
static void arp_note_off(sy_synth *s, int key);
static void arp_release(sy_synth *s);

/* the arpeggiator (B_ARP) turned on or off with notes sounding (a parameter change, sy_set_param; a patch
 * load goes through sy_all_off). Off: its arp_step is no longer run, so the note it plays (or the one about to start
 * and the one still sounding before it, the chord) ends here, and the keys it holds sound as plain notes (their
 * note-offs now come to note_off). On: the keys sounding as plain notes are handed to it (their note-offs now go to
 * arp_note_off, which would leave their voices gated). Before: such notes hung (gated, or the arpeggiator went on
 * playing keys long let go) until all notes off */
static void arp_switch(sy_synth *s, int on)
{
    int k, i, vel;
    if (!on) {
        if (s->arp_pc) {                /* (a step's note not started yet: the one before it still sounds) */
            if (s->arp_po >= 0) note_off(s, s->arp_po);
            s->arp_pc = 0; s->arp_po = -1; s->arp_note = -1;
        }
        arp_release(s);
        s->arp_on = 0;
        for (k = 0; k < 128; k++)
            if (s->arp_vel[k]) {
                vel = s->arp_vel[k];
                s->arp_vel[k] = 0;
                arp_held--;
                note_on(s, k, vel);
            }
        s->arp_n = 0;
        return;
    }
    for (k = 0; k < 128; k++) {
        if (!s->held[k]) continue;
        vel = 100;
        for (i = 0; i < SY_NV(s); i++) if (s->v[i].on && s->v[i].gate && s->v[i].key == k) vel = s->v[i].vel;
        s->kcount[k] = 0;               /* (mono: a key struck twice ends at once) */
        note_off(s, k);
        arp_note_on(s, k, vel);
    }
}

SY_COLD static void apply_event(sy_synth *s, const sy_event_t *e)
{
    int ty = e->st & 0xF0, ch = e->st & 0x0F;
    if (e->st == SY_EV_PARAM) { sy_set_param(s, e->d1, e->d2); return; }
    if (e->st == 0xFA || e->st == 0xFB) {           /* MIDI clock start / continue: the step grid counts from here */
        s->xclk_on = 1; if (e->st == 0xFA) s->xclk_ticks = 0;
        s->xclk_frames = -1e9f;         /* (the start to the first tick is no spacing: read, the arp ignores it) */
        s->xclk_n = 0; s->xclk_arp = 0.f; return;
    }
    if (e->st == 0xFC) { s->xclk_on = 0; return; }
    if (e->st == 0xF8) {                            /* a clock tick (24 per beat): the tempo */
        if (s->xclk_frames > 0.f && s->xclk_frames < s->fs) {
            float p0 = 60.f * s->fs / (24.f * ((float)s->patch[B_TEMPO] + 63.f));
            s->xclk_per = s->xclk_per > 0.f ? 0.75f * s->xclk_per + 0.25f * s->xclk_frames : s->xclk_frames;
            if (s->xclk_n == 0u) s->xclk_arp = 2.f * p0 * s->xclk_frames / (p0 + s->xclk_frames);   /* the tempos' mean */
            else if (s->xclk_n < XCLK_SETTLE) s->xclk_arp += XCLK_GLIDE * (s->xclk_frames - s->xclk_arp);
            else s->xclk_arp = 0.5f * (s->xclk_arp + s->xclk_frames);
            if (s->xclk_n < XCLK_SETTLE) s->xclk_n++;
        }
        s->xclk_frames = 0.f;
        s->xclk_on = 1;
        s->xclk_ticks++;
        return;
    }
    if (e->st >= 0xF0) return;
    if (s->channel >= 0 && ch != s->channel) return;   /* single mode, one channel */
#ifdef PTAB_DEBUG
    if (ty == 0x90 || ty == 0x80) fprintf(stderr, "ev blk %u %02x %d %d\n", s->block, ty, e->d1, e->d2);
#endif
    switch (ty) {
    case 0x90:
        if (e->d2) {
            if (s->patch[B_ARP]) arp_note_on(s, e->d1, e->d2);
            else note_on(s, e->d1 & 127, e->d2);
            break;
        }
        /* velocity 0: a note off */
        if (s->patch[B_ARP]) { arp_note_off(s, e->d1); break; }
        note_off(s, e->d1 & 127);
        break;
    case 0x80:
        if (s->patch[B_ARP]) { arp_note_off(s, e->d1); break; }
        note_off(s, e->d1 & 127);
        break;
    case 0xB0:
        s->cc[e->d1 & 127] = e->d2;
        s->sm_dirty = 1;
        if (!(s->patch[B_SMOOTH] & 1u) || !s->cc_seen[e->d1 & 127]) { s->ccs[e->d1 & 127] = (float)e->d2; s->cc_inc[e->d1 & 127] = 0.f; }
        else cc_dirty = 1;              /* (it glides from this block on) */
        s->cc_seen[e->d1 & 127] = 1;
        if (e->d1 == 120 || e->d1 == 123) sy_all_off(s);
        else if (e->d1 != 1 && e->d1 != 64 && e->d1 < 120) sy_set_param(s, e->d1, e->d2);   /* page A = the controllers */
        break;
    case 0xE0: {
        int b = (e->d2 << 7 | e->d1) - 8192;
        s->bend_t = b >= 0 ? (float)b / 8191.f : (float)b / 8192.f;
        if (!(s->patch[B_SMOOTH] & 1u) || !s->bend_seen) s->bend = s->bend_t;
        s->bend_seen = 1;
        break;
    }
    case 0xD0: s->pressure = (float)e->d1 / 127.f; break;
    default: break;
    }
}

/* ---- the arpeggiator (open.json arp: 1 up, 2 down, 3 up and down without repeating the
 * ends, 4 as played (starting on the second note), 5 random, 6 the chord; the rhythm below arp_fire). Its held
 * keys are its own: the voices it plays go through note_on/note_off. */
SY_COLD static void arp_note_on(sy_synth *s, int key, int vel)
{
    int first = s->arp_n == 0;          /* the first key after all were released restarts the arp, also
                                         * while the last note still plays its gate */
    key &= 127;
    if (!s->arp_vel[key] && s->arp_n < 16) s->arp_ord[s->arp_n++] = (uint8_t)key;
    if (!s->arp_vel[key]) arp_held++;
    s->arp_vel[key] = (uint8_t)(vel ? vel : 1);
    if (!s->arp_on || first) {
        s->arp_on = 1; s->arp_i = -1; s->arp_t = 1e9f; s->arp_dir = 1; s->arp_lt = 0.f;
#if ARP_CLK
        {   /* the first step waits for the next tick (1/48 beat) of the part's free-running clock (read: the
             * first step comes 1-4 blocks after the key at tempo 64, by the time since load, in a 3.6-block cycle) */
            float tpb = s->bdt * arp_bpm(s) * (48.f / 60.f);         /* ticks per block */
            float tk = ARP_CLK_OFF + s->arp_clk + ARP_CLK_B * tpb + 4.f;
            tk -= (float)sy_floori(tk);
            s->arp_wait = (1.f - tk) * 60.f / (arp_bpm(s) * 48.f);
        }
#endif
    }
}

SY_COLD static void arp_note_off(sy_synth *s, int key)
{
    int i, j;
    key &= 127;
    if (s->arp_vel[key]) arp_held--;
    s->arp_vel[key] = 0;
    for (i = j = 0; i < s->arp_n; i++) if (s->arp_ord[i] != key) s->arp_ord[j++] = s->arp_ord[i];
    s->arp_n = (uint8_t)j;
}

static void arp_release(sy_synth *s)
{
    int i;
    s->keep_prev = ARP_KEEP_PREV && s->arp_marked && s->block - s->arp_mark <= 3u;     /* (read on the phases the next step's voices start with) */
    if (s->arp_chord) {
        for (i = 0; i < 128; i++) if (s->arp_chord_k[i]) { note_off(s, i); s->arp_chord_k[i] = 0; }
        s->arp_chord = 0;
    }
    if (s->arp_note >= 0 && !s->arp_pc) note_off(s, s->arp_note);
    s->arp_pc = 0;
    s->arp_note = -1;
    s->keep_prev = 0;
}

/* the next note of the sequence (modes 1-6) */
static void arp_fire(sy_synth *s, const int *notes, int n, int oct, int mode)
{
    int total = n * oct, idx, key, i;
    if (mode == 6) {                    /* the chord */
        for (i = 0; i < n; i++) { note_on(s, notes[i], s->arp_vel[notes[i]]); s->arp_chord_k[notes[i]] = 1; }
        s->arp_chord = 1;
        return;
    }
    if (mode == 5) idx = (int)(sy_rand(&s->ch_rng) >> 8) % total;
    else if (mode == 3 && total > 1) {
        s->arp_i += s->arp_dir;
        if (s->arp_i >= total) { s->arp_i = total - 2; s->arp_dir = -1; }
        if (s->arp_i < 0) { s->arp_i = 1; s->arp_dir = 1; }
        idx = s->arp_i;
    } else {
        if (mode == 4 && s->arp_i < 0) s->arp_i = 0;   /* (as played: the first step is the second note) */
        s->arp_i = (s->arp_i + 1) % total;
        idx = mode == 2 ? total - 1 - s->arp_i : s->arp_i;
    }
    key = notes[idx % n] + 12 * (idx / n);
    if (key > 127) key = 127;
    /* the note sounds ARP_PC blocks after its step (re-read with the first step on the tick clock: 1);
     * the gate's end stays where it is */
    s->arp_po = s->arp_pc ? -1 : s->arp_note;     /* legato: the previous note ends when this one starts */
    s->arp_pk = key; s->arp_pv = s->arp_vel[notes[idx % n]]; s->arp_pc = ARP_PC;
    s->arp_note = key;
}

static void arp_pending(sy_synth *s)
{
    if (s->arp_pc && --s->arp_pc == 0) {
        if (s->arp_po >= 0 && s->arp_po != s->arp_pk) note_off(s, s->arp_po);
        s->arp_po = -1;
        note_on(s, s->arp_pk, s->arp_pv);
    }
}

/* the rhythm (arp.json, addendum-1.md): pattern 0 (and 62-64) an eighth per step, the gate 17 ms + 1.83 ms per
 * note length step (at the init tempo, in proportion to the step), 127 legato. Patterns 1-61 as measured (two bars
 * of ticks, 6 per sixteenth, sy_arp_tab.c), their gates scaled by the same law against length 64 (ASSUMPTION: the
 * law of pattern 0 for every pattern; pattern 26 at length 0 / 127 reads within ~30 %). Swing: no effect measured
 * with pattern 0; not applied */
/* pattern 0's gate (seconds): a fixed 2.3 ms plus 0.78 % of the step per length value, at least 14.3 ms
 * (measured against clocks 3-9: the fixed part does not scale with the step) */
/* corrected (read on the voices' releases, lengths 0-120 at two tempos, within a block): from the note's
 * start (2 blocks after the step) the gate lasts (0.00787 L + 0.0211) x step - 0.3 blocks, at least 0.055 x step (the 0.3 less half a block for the check at the block's end) */
#define ARP_GATE(step, L) (sy_maxf(0.055f * (step), (0.00787f * (float)(L) + 0.0211f) * (step) - 0.8f * s->bdt) + 2.f * s->bdt)
static void arp_step(sy_synth *s)
{
    arp_pending(s);
    int notes[16], n = 0, k, oct = s->patch[B_ARP_OCT] + 1, mode = s->patch[B_ARP], clk = s->patch[B_CLK_ARP];
    int pat = s->patch[B_ARP_PAT], L = s->patch[B_ARP_LEN];
    float bpm = arp_bpm(s), step, gl = 0.0667f + 0.00718f * (float)L;
#if ARP_CLK
    if (s->arp_wait > 0.f && s->arp_t >= 1e8f) {
        s->arp_wait -= s->bdt;
        if (s->arp_wait > 0.f) return;
    }
#endif
    if (mode == 4) for (k = 0; k < s->arp_n; k++) notes[n++] = s->arp_ord[k];
    else if (arp_held) for (k = 0; k < 128 && n < 16; k++) if (s->arp_vel[k]) notes[n++] = k;   /* (none held: no scan) */
    if (oct > 4) oct = 4;
    step = 60.f / bpm * CLK_Q[clk >= 1 && clk <= 19 ? clk : 4];
    if (!n) {   /* with no key held the sounding arp note plays to the end of its gate, then the arp stops */
        int done = 1;
        if (s->arp_on && (s->arp_note >= 0 || s->arp_chord)) {
            if (pat >= 1 && pat <= 61) {
                float end = s->arp_t + s->bdt / (step * (1.f / 12.f));
                done = s->arp_end < 0.f || end >= s->arp_end;
                s->arp_t = end;
            } else {
                s->arp_t += s->bdt;
                done = s->arp_t >= (L < 127 ? ARP_GATE(step, L) : step);
            }
        }
        if (done) {
            if (s->arp_on) arp_release(s);
            s->arp_on = 0;
        }
        return;
    }
    if (pat >= 1 && pat <= 61) {
        float tick = step * (1.f / 12.f), pos, end;
        const uint8_t (*ev)[2] = SY_ARP_EV[pat - 1];
        int ne = SY_ARP_N[pat - 1];
        if (s->arp_t >= 1e8f) { s->arp_t = 0.f; s->arp_k = 0; s->arp_end = -1.f; pos = 0.f; end = 0.f; }
        else { pos = s->arp_t; end = pos + s->bdt / tick; }
        if (s->arp_end >= 0.f && end >= s->arp_end) { arp_release(s); s->arp_end = -1.f; }
        while (s->arp_k < ne && (float)ev[s->arp_k][0] <= end) {
            float g = (float)ev[s->arp_k][1] * 0.25f * gl * (1.f / 0.526f);
            arp_release(s);
            arp_fire(s, notes, n, oct, mode);
            s->arp_end = (float)ev[s->arp_k][0] + sy_maxf(g, 0.017f / tick);
            s->arp_k++;
        }
        if (end >= 192.f) { end -= 192.f; s->arp_k = 0; if (s->arp_end >= 0.f) s->arp_end -= 192.f; }
        s->arp_t = end;
        (void)pos;
        return;
    }
    s->arp_t += s->bdt;
    if (ARP_KEEP_PREV && L < 127 && !s->arp_marked && s->arp_t >= ARP_GATE(step, L) - ARP_MARK * s->bdt) {
        s->arp_mark = s->block - ARP_KB;  /* the release hands over the phases from before its gate's audible
                                         * end (read on the phases the next step's voices start with) */
        s->arp_marked = 1;
#ifdef PTAB_DEBUG
        fprintf(stderr, "mark blk %u arp_t %.4f gate %.4f step %.4f\n", s->block, s->arp_t / s->bdt, ARP_GATE(step, L) / s->bdt, step / s->bdt);
#endif
    }
    if (L < 127 && s->arp_t >= ARP_GATE(step, L)) arp_release(s);
    if (ARP_LATE ? s->arp_t >= step - 1e-6f : s->arp_t + s->bdt > step) {   /* the step fires in the block its time
                                         * falls in (read on the steps' notes at the tick clock's grid: ceil) */
        s->arp_t = s->arp_t >= 1e8f ? (ARP_LATE && ARP_CLK ? sy_clampf(-s->arp_wait, 0.f, s->bdt) : 0.f) : s->arp_t - step;
        if (L < 127 || mode == 6) arp_release(s);
        arp_fire(s, notes, n, oct, mode);
        s->arp_marked = 0;
    }
}

/* ---- LFOs (per block) */
#ifndef ARP_LFO_P0
#define ARP_LFO_P0 0.f
#endif
#ifdef SY_CTL_RAM
#define LFO_SH_ATTR SY_CTL __attribute__((noinline))   /* (one copy: voice_block's and fx_tail's) */
#else
#define LFO_SH_ATTR
#endif
LFO_SH_ATTR static float lfo_shape(const sy_lfo_t *l, float ph, int shape, float sym)   /* sym 0..1 (0.5 = symmetric);
                                         * ph: the phase read (l->ph, or a clocked LFO's own) */
{
    float y;
    switch (shape) {
    case 1: {                           /* triangle, the peak at sym */
        float pk = sy_clampf(sym, 0.0001f, 0.9999f);
        y = ph < pk ? -1.f + 2.f * ph / pk : 1.f - 2.f * (ph - pk) / (1.f - pk);
        break;
    }
    case 2: y = 1.f - 2.f * ph; break;  /* saw, falling */
    case 3: y = ph < 0.5f ? 1.f : -1.f; break;   /* high while the triangle rises (open.json lfo23) */
    case 4: y = l->sh_to; break;        /* sample & hold */
    case 5: y = l->sh_from + l->ph * (l->sh_to - l->sh_from); break;   /* sample & glide: on the phase that draws
                                         * (l->ph wraps: lfo_advance). 0.91: the read phase ph of a clocked LFO (half a
                                         * cycle against it) jumped by the glide's span where l->ph wrapped, a step on
                                         * every destination (a click on the chorus's delay). the glides and
                                         * the S&H's steps at the free phase's wraps, the clocked offset not applied to
                                         * them (not measured). Unclocked: ph is l->ph (the same bits) */
    default:
        if (shape >= 6) {               /* the waves, measured (sy_hyb_lfow.c) */
            const float *w = sy_hyb_lfow[shape > 67 ? 61 : shape - 6];
            float x = ph * 64.f;
            int i = (int)x;
            float fr = x - (float)i;
            y = w[i & 63] + fr * (w[(i + 1) & 63] - w[i & 63]);
            break;
        }
        {                               /* sine, bent by the symmetry */
        float sn = -sy_cos2pi(ph);
        if (sym < 0.5f) {
            float tr = ph < 0.5f ? -1.f + 4.f * ph : 3.f - 4.f * ph;
            float m = (0.5f - sym) * 2.f;
            y = sn + m * (tr - sn);
        } else {
            float k = (sym - 0.5f) * 60.f;
            y = sn * (1.f + k) / (1.f + k * sy_absf(sn));
        }
        break;
    }
    }
    return y;
}

SY_CTLI static void lfo_advance(sy_lfo_t *l, float hz, float dt, int envmode)
{
    l->ph += hz * dt;
    if (envmode) {
        if (l->ph >= 1.f) { l->ph = 1.f; l->done = 1; }
        return;
    }
    if (l->ph >= 1.f) {
        l->ph -= (float)sy_floori(l->ph);
        l->sh_from = l->sh_to;
        l->sh_to = sy_randf(&l->rng);
    }
}

SY_CTL static float lfo_hz(const sy_synth *s, float rate, int clk, int kf, float key)
{
    float hz;
    if (clk >= 1 && clk <= 19) hz = SY_FAR(tempo_bpm)(s) / 60.f / CLK_Q[clk];
    else hz = sy_tab(s, T_LFO_HZ, rate);
    if (kf) hz *= CEXP2((key - 59.3f) / 12.f * 0.0076f * (float)kf);   /* centre 59.3: mod.json lfo1_keyfollow */
    return hz;
}
#if UNI_LFO
/* CLEANER: a unison voice's free LFO 1 or 2 at its start (measured, black box: bass/unilfo3.py, PROVENANCE 11; a note
 * of 2, 3, 4, 6 voices, each voice's LFO read on its own pitch's level or L/R balance). Between the note's tuning
 * steps k (uni_step) the phase moves by 0.378 - UNI_LFO_T f per step (f the LFO's rate, 0.58-6.2 Hz): the table's
 * 0.375 a step (CLN_LFO_PH) less UNI_LFO_T f k; the voices on the upper side (x >= 0, the centre with them) lead
 * by (64 - B 100) / 128 cycle (B 100, unison LFO phase: 64 none, 32 a quarter, 0 and 127 a half; LFO 1 and 2 alike).
 * ASSUMPTION: a key-triggered or clocked LFO left as it was (not measured) */
static SY_NSL void uni_lfo_ph(const sy_synth *s, sy_voice_t *v, int i, float key)
{
    const float f = SY_FAR(lfo_hz)(s, (float)s->patch[i ? L2_RATE : L1_RATE], 0, s->patch[i ? L2_KF : L1_KF], key);
    float ph = v->lfo[i].ph - UNI_LFO_T * f * (float)uni_step(v->uni_n, v->uni_i);
    if (uni_x(v->uni_n, v->uni_i) >= 0.f) ph += (64.f - (float)s->patch[UNI_PHASE]) * (1.f / 128.f);
    v->lfo[i].ph = ph - (float)sy_floori(ph);
}
#endif

/* ---- envelopes (per block): linear attack, decay and release straight in dB, the sustain slope falling in dB/s,
 * rising linearly */
/* the filter envelope's attack has its own, slower scale (filt2.json env_path: a linear ramp of ~0.14 s at 32,
 * ~0.35 s at 48, ~0.7 s at 64; doubling per 16 steps; above 64 extrapolated, ASSUMPTION) */
SY_CTLI static float fenv_attack(float a)
{
    if (a <= 0.f) return 0.f;
    if (a < 16.f) return a / 16.f * 0.0438f;
    return 0.70f * CEXP2((a - 64.f) / 16.f);
}

#ifndef ENV_SUS_UP
#define ENV_SUS_UP 3.04f                /* (0.92) per second, full level 1 (the reference: 1 / 15400 a frame at 46,875 Hz) */
#endif
#ifndef ENV_SUS_FLOOR
#define ENV_SUS_FLOOR 1e-4f
#endif
SY_CTLI static uint16_t sus_q(float sus) { return (uint16_t)(sy_clampf(sus, 0.f, 1.f) * 65535.f + 0.5f); }
SY_CTL static void env_step(const sy_synth *s, sy_env_t *e, float a, float d, float sus, float slope, float r, int amp, float *mc)
{                                       /* (mc: the voice's memo of this envelope's dB factor) */
    float t = s->bdt, x;
    e->prev = e->lvl;
    e->knee = 1.f;
    if (e->stage == 1) {                /* the linear attack, sample-exact: it may end inside the block (the knee) */
        x = amp ? sy_tab(s, T_ATK, a) : fenv_attack(a);
        if (!amp && x < 1e-4f) { e->prev = e->lvl = 1.f; e->stage = 2; e->knee = 0.f; }   /* attack 0: full at once */
        else if (amp) {                 /* the curve of sy_atk_curve, sample-exact; attack 0 is full from the
                                         * first frame; the short ramps hold full level until frame 9 (the decay
                                         * starts there, read with decay 0) */
            float len = x > 0.f ? x * s->fs : 1e-3f, nb = t * s->fs, end;
            if (e->an == 0.f) e->al0 = e->lvl;
            /* MEASURED (1.00): an attack from a level above 0 (a mono note in the
             * release) runs at least 8 frames, a straight line from that level to full (attack 0, 1, 5 and 20 alike
             * from 0.95, 0.83, 0.77 of the sustain); from 0 (a new voice) attack 0 stays full at once, as before */
            if (e->al0 > 0.f && len < 8.f) len = 8.f;
            e->alen = len;
            end = sy_atk_end(e->al0, len);
            if (len < 5.f && end < 9.f) end = 9.f;
            if (end < e->an + nb) {
                e->knee = (end - e->an) / nb;
                if (e->knee < 0.f) e->knee = 0.f;
                e->lvl = 1.f; e->stage = 2; t *= 1.f - e->knee;
            } else { e->lvl = sy_atk_curve(e->an + nb - 1.f, e->al0, len); t = 0.f; }
            e->an += nb;
        } else {
            float need = (1.f - e->lvl) * x;
            if (need >= t) { e->lvl += t / x; t = 0.f; }
            else { e->lvl = 1.f; e->stage = 2; e->knee = need / s->bdt; t -= need; }
        }
    }
    if (t <= 0.f) return;
    switch (e->stage) {
    case 2:                             /* decay: straight in dB down to the sustain level */
        e->lvl *= memo_db2lin(mc, -60.f / sy_tab(s, T_T60, d) * t);
        if (e->lvl <= sus) {
            e->stage = 3; e->sq = sus_q(sus);
            e->fol = e->lvl < ENV_SUS_FLOOR && e->lvl < sus;   /* (0.92: a sustain level moved up after the decay
                                         * died away: it rises, as in the sustain stage; MEASURED: sustain 0 -> 64,
                                         * decay 60 ended long before: the straight rise; decay 80 still running at
                                         * 0.6 % of the level: at once. ASSUMPTION: the reference's decay ends at about
                                         * -80 dB, ENV_SUS_FLOOR) */
            if (!e->fol) e->lvl = sus;
        }
        break;
    case 3: {
        float sr = sy_tab(s, T_SLOPE, slope);
        const uint16_t q = sus_q(sus);
        /* MEASURED (0.92): a sustain level moved in the sustain stage (the matrix, an LFO
         * on AE SUS / FE SUS, a controller) moves the level: down at once, up in a straight line at ENV_SUS_UP (full
         * level a third of a second; the reference: the same rate from 0, 32, 64 to 64, 96, 127, whatever the
         * decay). Until 0.91 the level stayed where the decay had left it. A level never moved: as before (the same
         * bits). ASSUMPTION: the slope rests while the level follows; the filter envelope as the amp's */
        if (q != e->sq) { e->sq = q; e->fol = 1; }
        if (e->fol) {
            if (e->lvl >= sus) e->lvl = sus;
            else e->lvl = sy_minf(sus, e->lvl + ENV_SUS_UP * t);
            if (e->lvl == sus) e->fol = 0;
            break;
        }
        if (slope > 63.5f && slope < 64.5f) break;
        if (sr < 0.f) e->lvl *= memo_db2lin(mc, sr * t);
        else if (e->lvl < 0.992f) e->lvl = sy_minf(e->lvl + sr * t, 0.992f);   /* (it stops at -0.07 dB) */
        if (e->lvl < 1e-6f) e->lvl = 0.f;
        break;
    }
    case 4:
        e->lvl *= memo_db2lin(mc, -60.f / sy_tab(s, T_T60, r) * t);
        if (e->lvl < ENV_END) { e->lvl = 0.f; e->stage = 0; }     /* the voice ends ~88 dB under full (read:
                                         * the six-voice step ends there, releases 40 and 60) */
        break;
    default: e->lvl = 0.f; break;
    }
}

/* ---- one voice's block: sources, matrix, envelopes, LFOs, then every value the audio needs */
#define E(b) (s->pv[b] + m[b])

SY_CTL static float src_value(const sy_synth *s, const sy_voice_t *v, int src)
{
    if (src <= 0) return 0.f;
    if (src == 1) return s->bend;
    if (src == 2) return s->pressure;
    if (src <= 18) {                    /* /128, 127 full (measured: the slope breaks at CC 64 and 96, 2 and 3
                                         * semitones at amount 66; 127 jumps to the full 4) */
        float c = s->ccs[SRC_CC[src - 3]];
        return c >= 127.f ? 1.f : c * (1.f / 128.f);
    }
    if (src == 19) return v->aenv.lvl;
    if (src == 20) return v->fenv.lvl;
    if (src <= 23) return 0.5f * v->lfo[src - 21].out;
    if (src <= 25) return (float)v->vel / 127.f;
    if (src == 26) return ((float)v->key - 60.f) / 64.f;      /* matrix.json keyfollow: 0.75 steps per octave */
    return 0.5f;
}

/* The matrix's pitch (osc 1 and 2 semitone): the summed semitones become a ratio through a table of just intervals
 * per semitone, interpolated linearly in cents (matrix.json law: +1 step = 9/8, -1 = 7/8, +-4 = 8/5 and 5/8; the CC
 * sweep's half steps). No grid: the pitch moves smoothly with every CC step (measured, 0.5 cent). */
static const float JI_C[13] = {0.f, 105.03f, 203.97f, 315.84f, 386.04f, 497.99f, 582.11f, 701.99f,      /* read per */
    813.88f, 884.04f, 968.62f, 1088.04f, 1200.f};                                                         /* semitone */

SY_CTLI static float ji_semi(float x)
{
    int o = sy_floori(x * (1.f / 12.f)), i;
    float r = x - 12.f * (float)o, f;
    i = (int)r;
    if (i > 11) i = 11;
    f = r - (float)i;
    return 12.f * (float)o + 0.01f * (JI_C[i] + f * (JI_C[i + 1] - JI_C[i]));
}

/* the matrix's slots: 1-3 (slot 2 with two destinations, slot 3 with three), slots 4-6 at B 103-111 exactly as slot
 * 1 (addendum-1.md); then the two LFO assigns (matrix, fx_tail) */
/* MEASURED (1.00): amount 127 counts as 128, the controller sources' 127 as 1 (src_value):
 * the mod wheel at 127 / 96 / 32 with amount 127 gives the static +127 (128 limited) / 96 / 32 exactly (cutoff, pan,
 * the dry level), at 127 with amount 126 +124, at 32 with amount 1 -31.5; until 0.99 amount 127 gave +126 */
#define MX_AMT(a) ((a) >= 127.f ? 64.f : (a) - 64.f)
static const uint8_t SLOT[9][3] = {{64, 65, 66}, {67, 68, 69}, {67, 70, 71}, {72, 73, 74}, {72, 75, 76}, {72, 77, 78},
                                   {103, 104, 105}, {106, 107, 108}, {109, 110, 111}};
SY_CTL static int matrix(const sy_synth *s, const sy_voice_t *v, float *m, uint8_t *touched)   /* the offsets added into
                                         * m; touched: the slots written (at most 11), returned their count */
{
    int nt = 0;
    int i;
    for (i = 0; i < 11; i++) {
        int src, dst, b;
        float amt, x;
        if (i < 9) { src = s->patch[PB + SLOT[i][0]]; dst = s->patch[PB + SLOT[i][1]]; amt = s->pv[PB + SLOT[i][2]]; }
        else { src = i == 9 ? 21 : 22; dst = s->patch[i == 9 ? B_L1_DEST : B_L2_DEST]; amt = s->pv[i == 9 ? B_L1_AMT : B_L2_AMT]; }
        if (!src || !dst || dst >= 120) continue;
        b = dst == 57 ? B_L3_RATE : dst == 58 ? B_L3_AMT : DEST[dst];
        if (!b) continue;
        x = src_value(s, v, src) * MX_AMT(amt) * 2.f;      /* 2 parameter steps per amount step */
        m[b] += x;
        touched[nt++] = (uint8_t)b;
    }
    /* MEASURED (0.92): the parameter and its matrix offsets are limited to 0..127 together
     * (the mod wheel full at amount 0 / 127 on every destination against the static patch at 0 / 127: the same sound
     * in the reference for the volumes, pan, transpose, the oscillators, cutoff, keyfollow, the envelopes and the LFOs'
     * amounts; until 0.91 the sum went past the range: a cutoff, a keyfollow, LFO 2's cutoff amounts or LFO 1's gain
     * too far, and through LFO 2's pan a pan clamped away). ASSUMPTION: the slots on one destination are summed first,
     * then limited. The effects' bytes (105-119) keep their own limit, 0..128 (sy_fx.c fx_vals); the oscillators'
     * keyfollow (21, 31) is left as it was (MEASURED: the reference's goes on past 127 there: amount 112 and 127 the
     * same, unlike the static 127). In range: the same offset (the same bits) */
    for (i = 0; i < nt; i++) {
        const int b = touched[i];
        float e;
        if ((b >= 105 && b <= 119) || b == O1_KF || b == O2_KF) continue;
        e = s->pv[b] + m[b];
        if (e > 127.f) m[b] = 127.f - s->pv[b];
        else if (e < 0.f) m[b] = -s->pv[b];
    }
    return nt;
}

#ifndef O3_G
#define O3_G 1.68f
#endif
#ifndef UNI_WIDE
#define UNI_WIDE 1.47f
#endif
#ifndef UNI_SHIFT
#define UNI_SHIFT 0.98f
#endif
#ifndef UNI_PHOFF
#define UNI_PHOFF 2748779069u                 /* 0.64 cycle */
#endif
#ifndef NZ_LP0
#define NZ_LP0 290.f
#endif
#ifndef NZ_LPS
#define NZ_LPS 2.418f
#endif
#ifndef SUB_G
#define SUB_G 0.183f
#endif
#ifndef LAD_HIR
#define LAD_HIR 1
#endif
#ifndef LAD_FCOR
#define LAD_FCOR 1
#endif
#ifndef LAD_FCOR_R
#define LAD_FCOR_R 30.f                 /* (full from resonance 30; the corner at 0 stays as read) */
#endif
#ifndef LAD_GIN
#define LAD_GIN (7.681f / 0.607f * 0.45f) /* the analog loop's input drive and output (sine over the level, read
                                         * from the input: here re the filter's own input, the input's -4.3 dB and the
                                         * -4.0 dB after the filter taken out; x0.45 in and x2.2 out with the stages'
                                         * own saturation, LAD_STS) */
#endif
#ifndef LAD_DRV
#define LAD_DRV 21.f
#endif
#ifndef LAD_C
#define LAD_C 1.4f                      /* at high resonance the loop limits 2.9 dB higher (1 up to resonance 96, 1.4 from 116),
                                         * the self-oscillation louder without building up faster */
#endif
#ifndef LAD_CD
#define LAD_CD 0.13f                    /* and the output falls by 0.13 of the drive more there (read: resonance
                                         * 104-127, osc volume 64-110, held notes: the self-oscillation within 1.5 dB)
                                         * */
#endif
#ifndef LAD_CR0
#define LAD_CR0 96.f
#endif
#ifndef LAD_CR1
#define LAD_CR1 116.f
#endif
#ifndef LAD_GCOR
#define LAD_GCOR 1.f
#endif
#ifndef LAD_DDF
#define LAD_DDF 0.33f
#endif
#ifndef LAD_GOUT
#define LAD_GOUT (0.10672f * 1.583f * 2.2f)
#endif

/* high oscillators are turned down (read with sines on both oscillators, keys and transpositions apart):
 * A by the oscillator's pitch alone (from pitch 98 on), and up to G when the key is high: between key 97.75 and the
 * point where G is reached the extra falls 0.46 dB per key. dB at pitch 96-127 */
static const float HI_A[32] = {0.f, 0.f, -0.05f, -0.32f, -0.49f, -0.77f, -1.06f, -1.37f, -1.77f, -1.96f, -2.2f, -2.71f,
    -3.04f, -3.37f, -3.99f, -4.37f, -4.61f, -5.02f, -5.61f, -6.1f, -6.99f, -7.33f, -7.76f, -9.1f, -9.8f, -10.56f,
    -12.63f, -13.72f, -14.51f, -16.05f, -18.61f, -21.38f};
static const float HI_G[32] = {0.f, 0.f, -0.16f, -0.99f, -1.53f, -2.39f, -3.12f, -4.05f, -5.f, -5.51f, -6.1f, -7.02f,
    -7.62f, -8.05f, -8.68f, -8.92f, -9.08f, -9.19f, -9.51f, -9.61f, -10.14f, -10.32f, -11.23f, -11.54f, -11.92f, -12.4f,
    -14.25f, -15.01f, -15.95f, -17.16f, -19.87f, -22.09f};
SY_CTLI static float hi_pitch(float hz) { return 69.f + 12.f * CLOG2(sy_maxf(hz, 1.f) * (1.f / 440.f)); }
SY_CTLI static float hi_gain(float hz, float pp)     /* pp = hi_pitch(hz), once per oscillator and block */
{
    float p = pp - 96.f, a, g, f, e;
    int i;
    if (p <= 0.f) return 1.f;
    if (p >= 31.f) { a = HI_A[31] + (p - 31.f) * 2.8f * -1.f; g = HI_G[31] + (p - 31.f) * -2.7f; }
    else {
        i = (int)p; f = p - (float)i;
        a = HI_A[i] + f * (HI_A[i + 1] - HI_A[i]);
        g = HI_G[i] + f * (HI_G[i + 1] - HI_G[i]);
    }
    (void)g; (void)e;              /* (the key's part: a dip filter, see hi_dip) */
    {   /* the table is the level at the output: undo the 18 kHz one-pole's loss at the fundamental */
        const float ka = 0.72389f, bb = 0.44778f;   /* K = tan(pi 18000 / 46875): a = K / (1 + K), b = (K - 1) / (K + 1) */
        float c = sy_cos2pi(sy_minf(hz * (1.f / 46875.f), 0.5f));
        float h2 = ka * ka * 2.f * (1.f + c) / (1.f + bb * bb + 2.f * bb * c);
        return CDB2LIN(a) / SY_FAR(sy_sqrt)(sy_maxf(h2, 1e-6f));
    }
}

SY_CTL static float pitch_st(const sy_synth *s, const sy_voice_t *v, float key)   /* the stretch (cents), per block */
{
    float st = SY_FAR(sy_stretch_cents)(key);
    if (v->iph) st = 0.19f + st * (float)v->iph * (1.f / 127.f);
    else st *= v->du;
    return st;
}
SY_CTLI static float pitch_hz(float note, float st)
{
    return 440.f * CEXP2((note - 69.f) * (1.f / 12.f) + st * (1.f / 1200.f));
}

SY_CTL static uint32_t hz_inc(const sy_synth *s, float hz)
{
    float x = hz * s->ifs;
    if (x > 0.49f) x = 0.49f;
    if (x < 0.f) x = 0.f;
    return (uint32_t)(x * 4294967296.f);
}

/* the morph weights of one oscillator for a shape value, a pulse width, a wave */
#include "sy_hyb_vol.h"
#include "sy_hyb_ring.h"

SY_CTLI static float hyb_ring_shape(float sh)
{
    float p = sy_clampf(sh, 0.f, 127.f) * 0.25f;
    int i = (int)p;
    if (i >= 31) return HYB_RING_SHAPE[31] + (sh - 124.f) / 3.f * (HYB_RING_SHAPE[32] - HYB_RING_SHAPE[31]);
    return HYB_RING_SHAPE[i] + (p - (float)i) * (HYB_RING_SHAPE[i + 1] - HYB_RING_SHAPE[i]);
}

/* (CLEANER, RING_SUB) the sub in the ring modulator (synth.c the mix, sy_voice.c sub_ring; measured there): sets
 * b->ring_s, the sub's factor in the ring's product: the ring's gain x osc 2's ring factor x the sub as in the mix over
 * osc 1's gain at osc volume 64 (balance 0; the 6-voice gain the sub's too), at osc 1's ring factor for a sine
 * whatever osc 1's shape (the sub's product the same with osc 1 a sine, saw or square), -1.0 dB (the lines read
 * against the reference with osc 1 a sine); and b->sub_gb, the sub's gain in the mix while the ring is on: osc 1's
 * side of the balance x the osc volume's law once more (not in the split routing). ov: the mix's osc volume gain (with
 * the 6-voice gain), hg1 osc 1's read level, b->g1 = balance x ov x hg1 (as set). In the flash: per block of a voice
 * with the ring and the sub on only */
B2_LATE static void ring_sub_set(const sy_synth *s, sy_vblk_t *b, float ov, float hg1, float ovol)
{
    float g0 = s->tables[T_SHAPE_G][0], t64 = s->tables[T_OSCVOL][64];
    float ovr = sy_tab(s, T_OSCVOL, sy_clampf(ovol, 0.f, 127.f));   /* (the volume's own law, without the 6-voice gain) */
    float ovn = ovr > 0.f ? t64 * ov / ovr : t64;
    float r1 = B2_FEXP2(0.631f * B2_FLOG2(sy_maxf(g0, 1e-6f) / s->tables[T_SHAPE_G][64])) / sy_maxf(g0, 1e-6f) * hyb_ring_shape(0.f);
    b->ring_s = b->ring_g * b->o2.ring * r1 * 0.891f / sy_maxf(ovn * hg1, 1e-6f);
    b->sub_gb = s->patch[ROUTE] == 3 ? 1.f : b->g1 / sy_maxf(ov * hg1, 1e-9f) * ovr / t64;
}

typedef struct { float shape, ws, wsaw, c, raw, ring; } osc_sc_t;   /* an oscillator's values per shape, cached */
SY_CTL static void osc_setup(const sy_synth *s, sy_osc_t *o, osc_sc_t *sc, float shape, float pw, int wave, int slot, float hz)
{
    float duty;
    shape = sy_clampf(shape, 0.f, 127.f);
    pw = sy_clampf(pw, -127.f, 127.f);              /* below 0 the duty falls under 0.5 (open.json velocity, lfo_dest) */
    o->inc = hz_inc(s, hz);
    o->dtw = sy_maxf(1e-6f, hz * s->ifs);         /* polyBLEP one sample wide (+ the 18 kHz roll-off: osc.json bandlimit) */
    o->idtw = 1.f / o->dtw;
    sy_blep_window(o->dtw, &o->blo, &o->bspan);   /* (the phases where blep() is surely 0) */
    if (sc->shape != shape) {           /* (the shape's part, when it changed: the tables and the ring's power law) */
        float g = sy_tab(s, T_SHAPE_G, shape), a = sy_tab(s, T_SHAPE_A, shape);
        sc->shape = shape;
        sc->ws = g * (1.f - a);
        sc->wsaw = g * a * 1.57079633f;             /* pi/2: the falling saw 1 - 2t with a unit fundamental */
        sc->c = sy_tab(s, T_SHAPE_C, shape);
        if (shape >= 64.f) {    /* CLEANER: the morph from 64 on, c = a u / (1 - b u), u = (shape - 63.5) / 63.5 (64
                                 * already holds half a step of the square): a 0.75, b 0.25, fitted to the development engine's
                                 * 2nd harmonic against the fundamental (black box: tools/measure/meas_shape.py,
                                 * fit_shape.py, within 2e-5) */
            float u = (shape - 63.5f) * (1.f / 63.5f);
            sc->c = 0.75f * u / (1.f - 0.25f * u);
        }
        /* the ring modulator's factor: the product follows the shapes' levels with the power 0.631 (measured
         * 3_ringmod, two sines: +9.4 dB over the unit-fundamental product, the shape gains' ratio being +14.9 dB;
         * ASSUMPTION: a power law between these two points); 1 / g at the saw, where 007 ring_only fits */
        sc->raw = 1.f / sy_maxf(g, 1e-6f);
        sc->ring = CEXP2(0.631f * CLOG2(sy_maxf(g, 1e-6f) / s->tables[T_SHAPE_G][64])) / sy_maxf(g, 1e-6f)   /* (sy_tab at 64) */
            * hyb_ring_shape(shape);                /* measured per shape */
    }
    o->ws = sc->ws; o->wsaw = sc->wsaw; o->c = sc->c; o->raw = sc->raw; o->ring = sc->ring;
    duty = 0.5f + pw / 254.f;                       /* duty 0.5 + pw/254 as the share above the mean (after the
                                                     * part's inversion: osc.json pw) */
    o->d = (uint32_t)(duty * 4294967040.f);
    o->wave = wave;
    o->wt = 0;
    o->tkn = 0.f;
    if (wave >= 2 && wave <= 63) {                  /* the table level whose top harmonic stays below 0.45 fs */
        int lvl = 0;
        while (lvl < SY_WAVE_LEVELS - 1 && (float)(64 >> lvl) * hz > 0.45f * s->fs) lvl++;
        if (s->wt[slot].id == wave) o->wt = s->wt[slot].tab[lvl];
        else o->wave = 0;
    }
}

/* cutoff 2's offset to cutoff 1 with the link on, in cutoff steps: half a step per value up to 16, then 1.5,
 * then 2 per value from 32, 4 from 48 (read from filter 2's ringing frequency and its static low pass) */
SY_CTLI static float link_off(float d)
{
    float a = sy_absf(d), r;
    r = a <= 16.f ? 0.5f * a : a <= 32.f ? 8.f + 1.5f * (a - 16.f) : a <= 48.f ? 32.f + 2.f * (a - 32.f) : 64.f + 4.f * (a - 48.f);
    return d < 0.f ? -r : r;
}

SY_CTL static float onepole_a(const sy_synth *s, float hz)
{
    float x = SY_2PI * hz * s->ifs;
    return x >= 3.f ? 1.f : 1.f - CEXP2(-x * 1.44269504f);
}

/* (in two halves: the cutoff's g, and the resonance's k, h and pg for that g: filter 2's g is the same for every
 * resonance it is set with in a block) */
SY_CTL static float filt_g(const sy_synth *s, float steps, int f1)
{
#ifndef CLN_LAW_ONLY
    if (f1) {   /* CLEANER: the frequency per cutoff value (sy_cln_filt.h: 1/100 cent, linear between the values, the
                 * ends held below 0 and above 127), measured on the development engine as a black box: a factor on the frequency
                 * after sy_cut_hz's limit to the audio band, as the development engine behaves. At 44.1 kHz (the FM-1) that limit
                 * (0.489 fs) falls below cutoff 127; applying the correction after the limit keeps the resonance
                 * at cutoff 126.8-127 within 8 cents (tools/measure/filt_dyn.py). Away from the limits
                 * the correction is added to the map's log2 Hz, one exp2 */
        float x = steps + 64.f, q = sy_clampf(steps, 0.f, 127.f), l, c, f, hi = sy_minf(23000.f, 0.489f * s->fs);
        int i, j = q >= 127.f ? 126 : (int)q;
        if (x < 0.f) x = 0.f;
        if (x > 255.99f) x = 255.99f;
        i = (int)x;
        l = s->cmap[i] + (x - (float)i) * (s->cmap[i + 1] - s->cmap[i]);
        c = ((float)CLN_FILT_CC[j] + (q - (float)j) * (float)(CLN_FILT_CC[j + 1] - CLN_FILT_CC[j])) * (1.f / 120000.f);
        f = CEXP2(l + c);
        if (f > 0.97f * hi || f < 25.5f) {  /* (near a limit: the correction is under 2.1 %) */
            float fl = CEXP2(l);
            if (fl > hi || fl < 24.7f) f = sy_clampf(fl, 24.7f, hi) * CEXP2(c);
        }
        return sy_tanpi(f * s->ifs);
    }
#else
    (void)f1;
#endif
    return sy_tanpi(sy_cut_hz(s, steps) * s->ifs);
}
SY_CTL static void filt_k(const sy_synth *s, float steps, float reso, float gg, float *h, float *k, float *pg, int f1)
{
    float kk;
    reso = sy_clampf(reso, 0.f, 127.f);
    kk = sy_tab(s, T_RESO_K, reso);
#ifndef CLN_LAW_ONLY
    if (f1) {   /* CLEANER: the damping per cutoff value, a factor at the resonances CLN_FILT_KR (linear between
                 * them), measured on the development engine as a black box (tools/measure/filt_fit.py, fit_filt.py, filt_top.py:
                 * sy_cln_filt.h); it falls from cutoff 117 on */
        float x = sy_clampf(steps, 0.f, 127.f), w, a0, a1, u;
        int i = x >= 127.f ? 126 : (int)x, j = 0;
        const float *ka, *kb;
        while (j < CLN_FILT_NR - 2 && reso > CLN_FILT_KR[j + 1]) j++;
        ka = CLN_FILT_K[j]; kb = CLN_FILT_K[j + 1];
        u = (reso - CLN_FILT_KR[j]) / (CLN_FILT_KR[j + 1] - CLN_FILT_KR[j]);
        w = x - (float)i;
        a0 = ka[i] + w * (ka[i + 1] - ka[i]);
        a1 = kb[i] + w * (kb[i + 1] - kb[i]);
        kk *= a0 + (a1 - a0) * u;
    }
#else
    (void)f1; (void)steps;
#endif
    *k = kk;
    *h = 1.f / (1.f + kk * gg + gg * gg);
    *pg = sy_tab(s, T_RESO_PG, reso);
}

/* the volume law of patch volume and part volume (inter.json cc_speed: 127 -> 40 is -24.6 dB; timing.md: 100 -> 127
 * is +6.8 dB): 0.189 dB per step plus 0.81 x the level in dB; 127 = unity */
/* the patch volume (A 91) has its own law (measured: dB re 127 at 32 / 64 / 100), piecewise
 * linear in dB; below 32 the power-law tail of a fit to the three points */

SY_CTLI static float hyb_tab(const float *t, float v)
{
    int i;
    if (v <= 0.f) return t[0];
    if (v >= 127.f) return t[127];
    i = (int)v;
    return t[i] + (v - (float)i) * (t[i + 1] - t[i]);
}

/* (CLEANER, 1.18C25: patch_gain, vd1_of and ncol in the flash, called through SY_FAR: on a change, or per block only
 * while modulated; their RAM to the oscillators) */
static SY_COLD __attribute__((noinline)) float patch_gain(float v)
{
    static const float V[4] = {32.f, 64.f, 100.f, 127.f}, D[4] = {-29.46f, -15.87f, -6.79f, 0.f};
    int i;
    if (v <= 0.f) return 0.f;
    if (v > 127.f) v = 127.f;
    if (v < 32.f) return FDB2LIN(D[0] + 0.1054f * (v - 32.f)) * FEXP2(1.616f * FLOG2(v / 32.f));
    for (i = 0; i < 2 && v > V[i + 1]; i++) ;
    return FDB2LIN(D[i] + (v - V[i]) / (V[i + 1] - V[i]) * (D[i + 1] - D[i]));
}

static float vol_gain(float v)
{
    if (v <= 0.f) return 0.f;
    if (v > 127.f) v = 127.f;
    return FDB2LIN(0.189f * (v - 127.f)) * FEXP2(0.81f * FLOG2(v / 127.f));
}

/* A unison voice's pan (0.92). MEASURED (the reference, two and four voices on separate pitches, each voice's left
 * and right level): the spread does not simply add to the pan. p is the voice's pan before
 * the spread (the patch's pan, velocity, the matrix, LFO 2's pan), t its slot's spread offset (spread x 128/127 x the
 * slot's place -1 .. 1). The offset is limited to +-64, and the voice moves on by (p - 64) |t| / 64: at the centre the
 * spread alone (as before: the same bits); towards a side the voices follow the pan, at a side all of them sit there
 * (pan 0 or 127: every voice hard left or right, whatever the spread). So a pan modulation (an LFO through the
 * matrix, LFO 2's pan) moves a wide unison sound as it moves a single voice; until 0.91 the voices were already at
 * the sides and the modulation was clamped away. ASSUMPTION: p limited to 0..127 for the (p - 64) term. Returns the
 * pan before the final limit to 0..127; t = 0 (no unison): p itself */
SY_CTLI static float uni_pan(float p, float t)
{
    float a;
    if (t == 0.f) return p;
    t = sy_clampf(t, -64.f, 64.f);
    a = sy_absf(t);
    return p + t + (sy_clampf(p, 0.f, 127.f) - 64.f) * a * (1.f / 64.f);
}
SY_CTLI static float pan_l(float p) { return p < 64.f ? 0.631f + 0.369f * (64.f - p) / 64.f : 0.631f * (127.f - p) / 63.f; }
SY_CTLI static float pan_r(float p) { return p > 64.f ? 0.631f + 0.369f * (p - 64.f) / 63.f : 0.631f * p / 64.f; }

SY_CTL static float vel_bip(float amount, int vel)     /* a bipolar velocity routing: 0 at velocity 1, full at 127 */
{
    return (amount - 64.f) * (64.f / 63.f) * ((float)vel - 1.f) / 126.f;
}

/* a velocity routing in steps of the destination: (amount - 64) x velocity / 63.5 (read on the pan and the
 * shapes: twice vel_bip's range, velocity 1 already moves one step at amount 127) */
#ifndef VFM_K
#define VFM_K 1.f                     /* ASSUMPTION: velocity -> FM in amount steps as the shapes; the reference goes
                                         * further at high amounts and velocities (past the FM maximum), not yet read */
#endif
SY_CTL static float vel_lin(float amount, int vel)
{
    return (amount - 64.f) * (float)vel * (1.f / 63.5f);
}

#if SY_CORES > 1
/* the control blocks on both cores (sy_render): while ctl_par is set, a voice's first block takes "another voice
 * sounds" from ctl_oth (decided before, in the voices' order), and the saturation's table is built after, in that
 * order (sy_render) */
static volatile int ctl_par;
static uint8_t ctl_oth[SY_MAX_VOICES];
#define CTL_PAR ctl_par
#else
#define CTL_PAR 0
#endif

/* ---- the control blocks' caches: values computed again only when their inputs changed (the same float operations
 * on the same inputs: the results are the same bits).
 * bc: values of the patch (pv, patch) and the rate alone, the same for every voice: filled once per block in
 * sy_render (blk_cache), before the voices' blocks, which may run on two cores; voice_block only reads it.
 * vc: per voice (and the static path's), keyed by the voice's own inputs: only that voice's block touches its entry. */
static struct sy_bc {
    float fs, col, vfe1, pvol, fade;    /* the keys */
    int punch;
    float olp_a, olp_b, op7;            /* by the rate: the oscillators' roll-off, onepole_a(19.7) */
    int n_mode;                         /* the noise colour */
    float n_lpa, n_lpb, n_hpa, n_gain, nf_gain;
    float vd1;                          /* velocity -> filter envelope 1: the amount's share, signed */
    float pg_base;                      /* patch_gain(pv[PATCH_VOL]) */
    float pk;                           /* the punch's size */
    float fd, fr;                       /* LFO 3's fade delay and rise */
}
#ifdef HYB_MULTI
bcs[2] = {{.punch = -1}, {.punch = -1}};   /* (two parts: one each) */
#define bc (bcs[s->pidx])
#else
bc = {.punch = -1};
#endif
enum { MM_AENV, MM_FENV, MM_GAIN, MM_FLO, MM_FHI, MM_BP1, MM_BP2, MM_DET2, MM_LGIN, MM_LKX, MM_LG5, MM_UNI, MM_UNK, MM_O3D, MM_O3M, MM_S0, MM_S1, MM_S2, MM_N };
SY_WORK_ATTR static struct {   /* (a firmware may give its pool: SY_WORK_POOL) */
    float st, p1, p2;                   /* the stretch and the two pitches (semitones): the keys of */
    float hz1, hz2, hp1, hp2, hg1, hg2, rate;   /* the frequencies, hi_pitch, hi_gain, clk_rate */
    float fa, ia, fb, ib;               /* the FM index's two parts per amount / envelope amount */
    osc_sc_t o[3];
    float mm[MM_N][2];                  /* the memos (memo_exp2, memo_db2lin) of the sites above */
    float dk, dp[5];                    /* the high keys' dip: its coefficients per key */
    float k6r, k6k, k2r, k2k, k2bp;     /* serial 6's and the analog modes' filter-2 damping per resonance (K6, K2A) */
    float wd, ww;                       /* the serial damping's weight per cutoff distance (W) */
} vc[SY_MAX_VOICES + 1];
/* log2 of constant tables, once (sy_init): the analog modes' corners, the holds' lengths, the one-poles' corners */
static const float LAD_LC[9][4] = {
    {371.1f, 0.550f, 0.0499f, -2.06f}, {625.4f, 0.550f, 0.0508f, -2.10f}, {1038.1f, 0.585f, 0.0528f, -1.89f},
    {1702.2f, 0.616f, 0.0545f, -1.70f}, {2710.4f, 0.680f, 0.0574f, -1.36f}, {4018.9f, 0.773f, 0.0630f, -0.87f},
    {5845.6f, 0.775f, 0.0632f, -0.87f}, {8211.9f, 0.553f, 0.0548f, -2.02f}, {10649.f, 0.242f, 0.0435f, -3.92f}};
static float LAD_LC_L2[9];
static const float SAT_H9[9] = {1.f, 4.42f, 7.15f, 11.54f, 18.72f, 30.20f, 48.80f, 78.98f, 120.12f};
static const float SAT_H10[9] = {2.73f, 4.42f, 7.15f, 11.54f, 18.72f, 30.20f, 48.80f, 78.98f, 120.12f};
static float SAT_H9_L2[9], SAT_H10_L2[9];
static const float SAT_T[22] = {20000.f, 13469.f, 8606.f, 6353.f, 4820.f, 3679.f, 2790.f, 2093.f, 1559.f,
    1144.f, 839.f, 606.5f, 441.f, 317.f, 228.6f, 163.9f, 117.6f, 84.3f, 60.2f, 43.2f, 30.8f, 22.1f};
static const float SAT_HK[22] = {0.00134041f, 0.00187658f, 0.00261381f, 0.00361913f, 0.00509361f, 0.00710431f, 0.00991938f, 0.0138742f, 0.0194384f, 0.02715f, 0.0381532f, 0.053063f, 0.0747326f, 0.104325f, 0.146811f, 0.20647f, 0.293018f, 0.421347f, 0.623348f, 0.99402f, 1.92506f, 8.60361f};
static float SAT_T_L2R[21], SAT_HK_L2R[21];   /* log2 of the steps' ratios */
static void ctl_cache_reset(void)
{
    int i, j;
#ifdef HYB_MULTI
    bcs[0].fs = bcs[1].fs = 0.f;
#else
    bc.fs = 0.f;                        /* (the rate's part is computed again at the next block) */
#endif
    for (i = 0; i <= SY_MAX_VOICES; i++) {
        vc[i].st = MEMO_NONE; vc[i].fa = vc[i].fb = -1.f;
        vc[i].dk = vc[i].k6r = vc[i].k2r = vc[i].wd = MEMO_NONE;
        for (j = 0; j < 3; j++) vc[i].o[j].shape = -1.f;
        for (j = 0; j < MM_N; j++) vc[i].mm[j][0] = MEMO_NONE;
    }
    for (i = 0; i < 9; i++) { LAD_LC_L2[i] = FLOG2(LAD_LC[i][0]); SAT_H9_L2[i] = FLOG2(SAT_H9[i]); SAT_H10_L2[i] = FLOG2(SAT_H10[i]); }
    for (i = 0; i < 21; i++) { SAT_T_L2R[i] = FLOG2(SAT_T[i + 1] / SAT_T[i]); SAT_HK_L2R[i] = FLOG2(SAT_HK[i + 1] / SAT_HK[i]); }
}
static SY_COLD __attribute__((noinline)) float vd1_of(float x)   /* velocity -> filter envelope 1 (B 54): the amount's share, signed */
{
    float vd = (x - 64.f) / 63.f;
    /* (the amount's share is |d|^1.43: filt2.json vel_fenv at 32 / 96 reads 0.37, not 0.5) */
    return vd < 0.f ? -FEXP2(1.43f * FLOG2(sy_maxf(-vd, 1e-6f))) : FEXP2(1.43f * FLOG2(sy_maxf(vd, 1e-6f)));
}
static SY_COLD __attribute__((noinline)) void ncol(const sy_synth *s, float col, struct sy_bc *o)   /* the noise colour's coefficients */
{
    if (col < 63.5f) {                      /* fitted to the band levels of oscillators.md */
        o->n_mode = 1;
        float K = FTANPI(sy_minf(0.45f * s->fs, NZ_LP0 * FEXP2(col / 32.f * NZ_LPS)) * s->ifs);
        o->n_lpa = K / (1.f + K);
        o->n_lpb = (K - 1.f) / (K + 1.f);
        o->n_gain = FDB2LIN(9.5f * FEXP2(0.4f * FLOG2(sy_maxf(1e-4f, 1.f - col / 64.f))));
    } else if (col > 64.5f) {
        o->n_mode = 2;
        o->n_hpa = SY_FAR(onepole_a)(s, 400.f * FEXP2((col - 96.f) / 31.f * 2.75f));
        o->n_gain = FDB2LIN(0.8f * (col - 64.f) / 63.f);
    } else { o->n_mode = 0; o->n_gain = 1.f; }
    /* the noise FM (mode 3) takes the colour's low pass but not its high pass, and a level of its
     * own (read every 16 colour steps on the bands next to osc 2's pitch, amount 48; 48 interpolated) */
    {
        static const float NFX[9] = {3.f, 3.5f, -4.5f, -2.25f, 0.f, -3.5f, -9.f, -4.5f, -8.f};
        float c4 = sy_clampf(col, 0.f, 127.f) * (8.f / 127.f);
        int ic = c4 >= 8.f ? 7 : (int)c4;
        o->nf_gain = (o->n_mode == 1 ? o->n_gain : 1.f) * FDB2LIN(NFX[ic] + (c4 - (float)ic) * (NFX[ic + 1] - NFX[ic]));
    }
}
#ifndef VB_FLASH
#define VB_FLASH 1                      /* (wave 3) the control blocks' code that runs only in some patches' blocks, or
                                         * once a block for the part, in the flash: voice_block's analog-mode and
                                         * saturation-type set-ups (lad_set, sat_set) and blk_cache (.ram_text: -4.4 KB;
                                         * the same arithmetic, the flash's copies of 2^x). 0: in RAM, as before */
#endif
#if VB_FLASH
#define VB_FN static SY_LATE
#define VB_CALL(f) SY_FAR(f)
#else
#define VB_FN SY_CTLI static
#define VB_CALL(f) f
#endif
#if VB_FLASH
static SY_LATE void blk_cache(const sy_synth *s)   /* (sy_render: once a block, mostly the keys' compares) */
#else
SY_CTL static void blk_cache(const sy_synth *s)
#endif
{
    int all = bc.fs != s->fs;
    if (all) {
        float K = sy_tanpi(sy_minf(18000.f, 0.45f * s->fs) * s->ifs);
        bc.fs = s->fs;
        bc.olp_a = K / (1.f + K);
        bc.olp_b = (K - 1.f) / (K + 1.f);
        bc.op7 = VB_CALL(onepole_a)(s, 19.7f);
    }
    if (all || bc.col != s->pv[NOISE_COL]) {
        bc.col = s->pv[NOISE_COL];
        SY_FAR(ncol)(s, bc.col, &bc);
    }
    if (all || bc.vfe1 != s->pv[B_V_FE1]) {
        bc.vfe1 = s->pv[B_V_FE1];
        bc.vd1 = SY_FAR(vd1_of)(bc.vfe1);
    }
    if (all || bc.pvol != s->pv[PATCH_VOL]) { bc.pvol = s->pv[PATCH_VOL]; bc.pg_base = SY_FAR(patch_gain)(bc.pvol); }
    if (all || bc.fade != s->pv[B_L3_FADE]) {
        bc.fade = s->pv[B_L3_FADE];
        bc.fd = sy_tab(s, T_FADE_D, bc.fade);
        bc.fr = sy_tab(s, T_FADE_R, bc.fade);
    }
    if (bc.punch != s->patch[B_PUNCH]) {   /* the start transient's size by the punch (0 none, 32 / 64 / 96
                                         * at 0.139 / 0.33 / 0.58 of 127) */
        static const float PK[5] = {0.f, 0.139f, 0.33f, 0.58f, 1.f};
        float pu = sy_clampf((float)s->patch[B_PUNCH], 0.f, 127.f) * (4.f / 127.f);
        int ip = pu >= 4.f ? 3 : (int)pu;
        bc.punch = s->patch[B_PUNCH];
        bc.pk = PK[ip] + (pu - (float)ip) * (PK[ip + 1] - PK[ip]);
    }
}

/* MEASURED: the part's effects take the matrix offsets of the voice started last (not a
 * sum, not the loudest), per block, held after that voice ends (another voice still on does not take over), cleared
 * by a patch load; a unison note's last voice. voice_block's, for that voice (in the flash: once a block) */
static B2_LATE int16_t off256(float x)  /* an offset in 1/256 steps (rm_off, dry_off), limited to +-127 */
{
    return (int16_t)(sy_clampf(x, -127.f, 127.f) * 256.f);
}
static B2_LATE void fx_take(sy_synth *s, const sy_voice_t *v, const float *m)
{
    int i, any = 0;
    NO_UNROLL for (i = 0; i < SY_FXN; i++) {   /* (A 105-119, then page B's: SY_FXB) */
        const float x = m[i < 15 ? 105 + i : SY_FXB[i - 15]];
        s->fxm[i] = x; any |= x != 0.f;
    }
    s->fx_mod = (uint8_t)any;
    s->rm_off = off256(m[PB + 99]);     /* (the part's input ring modulator and dry level: 96, 113) */
    s->dry_off = off256(m[B_OUT2]);
#ifdef HYB_SHIMMER
    {   /* (0.89) the shimmer's targets: the same law, source x (amount - 64) x 2 steps; LFO 3 (unipolar amount): its
         * output x amount. (1.18C45) LFO 3 on SHIM MOD: not an offset; its output (-1..1) x amount / 127 in o[2], the
         * shimmer's pitch modulator in place of the reverb's LFO */
        float o[3] = {0.f, 0.f, 0.f};
        NO_UNROLL for (i = 0; i < 12; i++) {
            const int r = s->shx_rt[i];
            int c;
            float a;
            if (r < 1 || r > 2) continue;
            if (i < 9) { c = s->patch[PB + SLOT[i][0]]; a = s->pv[PB + SLOT[i][2]]; }
            else if (i < 11) { c = i + 12; a = s->pv[i == 9 ? B_L1_AMT : B_L2_AMT]; }
            else { c = 23; a = 0.5f * s->pv[B_L3_AMT] + 64.f; }
            if (c) o[i == 11 && r == 2 ? 2 : r - 1] += SY_FAR(src_value)(s, v, c) * (a - 64.f) * 2.f;
        }
        s->shm[0] = o[0]; s->shm[1] = o[1]; s->shm[2] = o[2] * (2.f / 127.f);
    }
#endif
    s->fx_tail = !v->on;                /* (it ended in this block: fx_tail goes on from its LFOs, key and velocity) */
    if (!v->on) {
        const uint32_t *a = (const uint32_t *)(const void *)v->lfo;
        uint32_t *d = (uint32_t *)(void *)s->fx_lfo;
        NO_UNROLL for (i = 0; i < (int)(sizeof s->fx_lfo / 4u); i++) d[i] = a[i];   /* (a word loop: small code) */
        s->fx_vel = (uint8_t)v->vel;
        s->fx_key = (uint8_t)v->key;
    }
}

#if PW_PH
/* CLEANER: a change of the pulse width moves the oscillator's phase (measured, black box: the edges of a pulse under
 * LFO 1 -> pulse width, bass/edges.py, PROVENANCE 11): the wrap's edge moves against the width's by PW_PH_K x the
 * change of the duty x f / f(key) cycles (0.36-0.40 at keys 36-84, osc 1 and 2 alike, -24..+12 semitones, a bend
 * +2: the shift is a time, a share of the key's period); statically the edges stay where they were. d1, d2: the
 * duties' change since the last block, q1, q2: the pitches over the key (semitones). Spread over the block as a
 * frequency (no step). On the clock osc 1's cycle is whole ticks: its shift is the clock's (P ticks a cycle: s1 P
 * more in the block), osc 2's increment takes back what that adds to it. (Flash: only while a width moves) */
static SY_LATE void pw_ph(sy_vblk_t *b, float d1, float d2, float q1, float q2)
{
    const float s1 = -PW_PH_K * d1 * FEXP2(q1 * (1.f / 12.f)), s2 = -PW_PH_K * d2 * FEXP2(q2 * (1.f / 12.f));
    if (b->tk) {
        const float x1 = s1 * b->o1.tkn;   /* (osc 1's shift in ticks) */
        b->tk_r = sy_minf(0.999f, sy_maxf(0.f, b->tk_r + x1 * (0.5f / (float)SY_BLOCK)));
        b->tk_inc2 += (uint32_t)(int32_t)((s2 * 4294967296.f - x1 * (float)b->tk_inc2) / (2.f * b->tk_r * (float)SY_BLOCK));
    } else {
        b->o1.inc += (uint32_t)(int32_t)(s1 * (4294967296.f / (float)SY_BLOCK));
        b->o2.inc += (uint32_t)(int32_t)(s2 * (4294967296.f / (float)SY_BLOCK));
    }
}
#endif

#if DET_WANDER
/* CLEANER, MEASURED (PROVENANCE 9e; black-box renders, osc 2 alone, its pitch tracked over 20-120 s): with osc 2's
 * detune at 1-92 the detune is not still. Its amount moves around the measured one (T_DET2, the note's tuning step
 * and the keyboard's narrowing kept) by +-DW_K of the nominal amount (the step's factor left out: the same depth on
 * every note of a run), on every key and value alike (sd 0.118 x the amount, notes 36-84, detunes 16-92); 93 and up
 * it stands still, and osc 3's detune then too (osc 3's moves with osc 2's, mirrored: the same wander, correlation
 * -0.997, gated by osc 2's value). Its shape: a triangle of constant height whose rate drifts (half cycles 0.94-2.7 s,
 * mean 1.50, sd 0.37, neighbours correlated 0.54; spectrum centred at 0.342 Hz, width 0.097), not one per
 * voice (see below), free running (no restart at the note-on). The generator is this project's own fit to those
 * statistics: a triangle at DW_R0 x 2^(+-DW_OCT) (uniform), a new target rate every DW_T s +-50 %, followed with a
 * DW_TAU s lag. It moves osc 2's pitch by up to +-0.2 x its detune: detuned oscillators drift apart and together, the
 * beating speeds up and slows down (the patch "a test patch" at C2).
 * MEASURED (PROVENANCE 9f; osc 2 a sine, up to 16 keys held, unison voices hard left / right): the wander is not a
 * voice's own. The first voice slot (the one a note takes after the load or when all are free) has one; every other
 * slot moves with one common wander (correlation 1.00, no lag; each at its own depth), whatever the key, the note-on
 * time or the unison detune. A note's unison voices on slots 1.. move together (left and right
 * coupled), on slots 0 and 1 apart. So here: two generators a part, stepped once a block (dw_tick, the end of the
 * block): dw_w[0] for its first voice, dw_w[1] for the others. ASSUMED: free running whether used or not (the
 * reference's common wander is not a function of the time since the load alone: its history there unresolved);
 * a part of a multi build has its own pair and its own first voice */
#define DW_K 0.206f                     /* (the triangle's peak, of the nominal amount: sd 0.118 through the tracker) */
#define DW_R0 0.318f                    /* Hz */
#define DW_OCT 0.8f
#define DW_T 2.f
#define DW_TAU 0.5f
#define DW_OFF 92.5f                    /* osc 2's detune at and above 93: still */
static uint32_t dw_seed = 0x6C8E9CF5u;
B2_COLD static void dw_tick(sy_synth *s)   /* a block of the two wanders (the flash) */
{
    int i;
    const float dt = s->bdt;
    for (i = 0; i < 2; i++) {
        sy_dw_t *g = &s->dw[i];
        if ((g->n -= dt) <= 0.f) {      /* a new target rate (the first: the phase anywhere) */
            float r = DW_R0 * B2_FEXP2(DW_OCT * sy_randf(&dw_seed));
            if (g->r <= 0.f) { g->r = r; g->ph = 0.5f + 0.5f * sy_randf(&dw_seed); }
            g->t = r;
            g->n = DW_T * (1.f + 0.5f * sy_randf(&dw_seed));
        }
        g->r += (g->t - g->r) * dt * (1.f / DW_TAU);
        g->ph += g->r * dt;
        if (g->ph >= 1.f) g->ph -= 1.f;
        s->dw_w[i] = DW_K * (1.f - 4.f * sy_absf(g->ph - 0.5f));
    }
}
#endif

#ifndef F1_STALE
#define F1_STALE 1                      /* CLEANER: filter 1's modes 4 and 6 take the coloured noise (PROVENANCE 14b) */
#endif
#ifndef F1X_G
#define F1X_G 1.24f                     /* (mode 6's level: the noise at volume 127 + 18.0 dB, the reference's ratio; the
                                         * colour's shape and its errors as the noise's own: ncol) */
#endif
#ifndef F1X_G4
#define F1X_G4 0.0886f                  /* (mode 4 against mode 6: -21.05 dB; split's flat pass -21.1 dB) */
#endif
#if F1_STALE
/* CLEANER: filter 1 in mode 4 or 6 (serial, parallel) with the noise on (any volume) and coloured (B 39 not 64) puts out
 * the coloured noise alone, flat, at a level of its own whatever the noise volume, the oscillators, the cutoff or the
 * resonance (filter 2 filters it as usual); mode 4 21.05 dB under mode 6. In split filter 1 passes osc 1 (mode 6) or
 * F1X_G4 of it (mode 4), flat, whatever the cutoff and the resonance. The osc volume above 64 lowers both, 6.02 dB at
 * 127, linearly in dB (what it adds before the saturation it takes off after filter 1). Mode 6 with the distortion
 * (F1M6_DIST: b->m1 4) keeps mode 6's level here: the distortion after it gives -13.1 dB, the reference -13.5. Measured,
 * black box: tools/measure/f1stale.py, PROVENANCE 14b. (Flash: modes 4 and 6 only) */
static SY_LATE void f1_stale(const sy_synth *s, sy_vblk_t *b, float ovol)
{
    const float ovd = FEXP2(-sy_clampf((ovol - 64.f) * (1.f / 63.f), 0.f, 1.f));
    if (s->patch[ROUTE] == 3) { if (b->m1 == 4) b->m4_g = F1X_G4 * ovd; }
    else if (b->noise_g != 0.f && b->n_mode != 0) b->f1x = F1X_G * ((s->patch[F1_MODE] & 7) == 4 ? F1X_G4 : 1.f) * ovd;
}
#endif
#ifdef CLN_LFO_TAB
/* CLEANER: a voice that sounds with others (any voice on at its first block: held, releasing, or started in the same
 * block) starts its free LFOs 1 and 2 at a phase by its tuning step (CLN_LFO_PH, a cycle of 16); alone it keeps the
 * part's phase (measured on the development engine as a black box: tools/measure/lfo_meas.py). S1: out of voice_block's RAM code,
 * once a voice, in the flash */
static SY_NSL void lfo_pend_start(sy_synth *s, sy_voice_t *v, float key)
{
    int i, j, oth = 0;
    (void)key;
    v->lfo_pend = 0;
#if SY_CORES > 1
#ifdef HYB_MULTI
    if (ctl_par) oth = ctl_oth[SY_VI(s, v)]; else   /* (two parts: by the pool's index) */
#else
    if (ctl_par) oth = ctl_oth[v - s->v]; else
#endif
#endif
    for (j = 0; j < s->max_voices; j++) if (&s->v[j] != v && s->v[j].on) oth = 1;
    if (oth)
        for (i = 0; i < 2; i++)
            if (!s->patch[i ? L2_TRIG : L1_TRIG] && !s->patch[i ? L2_ENV : L1_ENV]
                && (s->patch[i ? B_CLK_L2 : B_CLK_L1] < 1 || s->patch[i ? B_CLK_L2 : B_CLK_L1] > 19)) {   /* (a clocked
                                         * LFO keeps the part's phase) */
#ifdef CLN_LFO_ZERO
                v->lfo[i].ph = 0.f;         /* (a measurement build: lfo_meas.py) */
#else
                v->lfo[i].ph = (float)CLN_LFO_PH[(v->lfo_step - 1u) & 15u] * (1.f / 65536.f);
#endif
#if UNI_LFO
                if (v->uni_n > 1) uni_lfo_ph(s, v, i, key);
#endif
            }
}
#endif

/* a voice's first note: its start phases (S1: out of voice_block's RAM code, in the flash) */
static SY_NSL void voice_start_phase(sy_synth *s, sy_voice_t *v)
{
    /* free running = the voice's oscillators stop while it is idle and continue from there. A voice's
     * first note starts each oscillator at an unrelated phase: the reference's first-note phases change with the
     * time since load (per block, irregularly) and from osc to osc, not with the key; they are not 0 (read with
     * sines, a note on 0-160 blocks after load). The phase 0 start gave every onset the largest low burst.
     * Osc 2 starts in phase with osc 1 (read: two sines at detune 0, balance 64, 16 note-ons over 2.4 s: always
     * the coherent sum, +4.0 dB re one osc; with detune the pair then drifts apart note by note). */
    uint32_t h = s->block * 0x9E3779B1u + (uint32_t)(v - s->v) * 0x85EBCA6Bu + (uint32_t)v->uni_i * 0xC2B2AE35u;
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    if (v->uni_n > 1 && UNI_SPREADPH) {
        /* (off by default) a note's unison voices start spread over the cycle, voice i at i/N, each
         * off by a random phase of sd UNI_PHJ cycle. Read: at unison detune 0 (sines, 48 note-ons 0.27 s apart)
         * the onset reads 0.2-2 dB under independent random phases and this fits within 0.44 dB (random 0.98);
         * but saws at detune 48 (32 note-ons) read at the power sum, random 0.33 dB, this 0.60 dB */
        uint32_t g = s->block * 0x2545F491u + (uint32_t)v->key * 0x9E3779B1u, j1 = h, j2 = h * 0x9E3779B1u, j3 = h * 0x85EBCA6Bu;
        float jit;
        g ^= g >> 15; g *= 0x7FEB352Du; g ^= g >> 13;
        jit = ((float)(j1 >> 8) + (float)(j2 >> 8) + (float)(j3 >> 8)) * (1.f / 16777216.f) - 1.5f;   /* sd 0.5 */
        h = g + (uint32_t)((double)v->uni_i / (double)v->uni_n * 4294967296.0) + (uint32_t)(int32_t)(jit * (UNI_PHJ / 0.5f) * 4294967296.0f);
    }
    v->ph1 = h;
    v->phs = h >> 1;                /* (the sub keeps its relation to osc 1) */
    v->ph2 = h;
    if (v->uni_n > 1 && v->uni_i != v->uni_n / 2) {   /* of a note's unison voices only the middle one
                                     * (slot N/2) starts its osc pair in phase; each other one has its own random
                                     * osc 2 offset (read: per voice line, sines at detune 0, 2-8 voices) */
        uint32_t q = h * 0x9E3779B1u + s->block * 0x85EBCA6Bu + (uint32_t)v->uni_i * 0x27D4EB2Fu;
        q ^= q >> 15; q *= 0x2C1B3C6Du; q ^= q >> 12;
        v->ph2 = h + q;
    } else
    if (!v->solo) {                 /* a tone joining held keys: osc 2 off osc 1 by a phase that changes
                                     * with the time, the same for every voice started then (read: 2- and 4-note
                                     * chords of sines at detune 0, 16 note-on times: tones 2-4 alike, spread) */
        uint32_t g = s->block * 0x2545F491u;
        g ^= g >> 15; g *= 0x7FEB352Du; g ^= g >> 13;
        v->ph2 = h + g;
    }
    v->ph3 = v->ph2 + 0x60000000u;  /* osc 3 (every mode) starts 3/8 cycle ahead of osc 2 */
}

/* the osc init phase with the matrix's offset (88; MEASURED: the twin's): the note's first block takes it and holds it
 * for the note (ASSUMPTION: read once); the start phases as voice_start sets them, where note_voice leaves them (no
 * phase table, no hand-over) (S1: out of voice_block's RAM code, in the flash) */
static SY_NSL void init_phase(const sy_synth *s, sy_voice_t *v, const float *m)
{
    const uint32_t q = (uint32_t)(v->iph = (uint8_t)(s->pv[B_INIT_PH] + m[B_INIT_PH] + 0.5f)) << 25;
    if (q && !PHASE_TAB && !v->used) { v->ph1 = v->phs = 0; v->ph2 = q; v->ph3 = 0u - q; }
}

/* the high keys' dip filter's coefficients for a key (S1: out of voice_block's RAM code, in the flash: once a note) */
static SY_LATE void dip_coef(const sy_synth *s, float *dp, float key)
{
    float gd = -5.45f * sy_minf(1.f, (key - 97.75f) * (1.f / 10.25f)), A = FDB2LIN(gd * 0.5f);
    float w = sy_minf(5046.f * s->ifs, 0.45f), cw = FCOS2PI(w), al = FSIN2PI(w) * (1.f / (2.f * 1.143f));
    float ia = 1.f / (1.f + al / A);
    dp[0] = (1.f + al * A) * ia; dp[1] = -2.f * cw * ia; dp[2] = (1.f - al * A) * ia;
    dp[3] = -2.f * cw * ia; dp[4] = (1.f - al / A) * ia;
}

/* portamento (S1: out of voice_block's RAM code, run while a glide is under way): exponential in pitch, the remaining
 * interval floored at 0.95 semitone, so the last semitone goes at a constant speed (mod.json portamento: within 2-5
 * cents; ASSUMPTION: the floor is absolute, the glides measured were 12 semitones), per block */
static SY_LATE void porta_step(const sy_synth *s, sy_voice_t *v, const float *m, float dt)
{
    float tau = sy_tab(s, T_PORTA, E(PORTA)), rc = 0.95f;
    if (tau < 1e-4f) v->glide = v->target;
    else {
        float r = v->target - v->glide, a = sy_absf(r), t = dt;
        if (a > rc) {
            float q = a * FEXP2(-t / tau * 1.44269504f);
            if (q >= rc) { a = q; t = 0.f; }
            else { t -= tau * FLOG2(a / rc) * 0.693147181f; a = rc; }
        }
        a -= t * rc / tau;
        v->glide = a <= 0.0005f ? v->target : v->target - (r < 0.f ? -a : a);
    }
}

/* (the memos in the flash: 2^x by the flash's copy, the same polynomial) */
static SY_INL float fmemo_exp2(float *c, float x) { if (c[0] != x) { c[0] = x; c[1] = FEXP2(x); } return c[1]; }
static SY_INL float fmemo_db2lin(float *c, float x) { if (c[0] != x) { c[0] = x; c[1] = FDB2LIN(x); } return c[1]; }
/* filter 1 in an analog mode (5, 7): filter 2's own damping and the ladder's coefficients (voice_block; per block
 * while the mode is on: in the flash, VB_FLASH) */
VB_FN void lad_set(const sy_synth *s, sy_vblk_t *b, const float *m, int vi, float c1, float r1, float r2)
{
    {   /* with filter 1 in an analog mode, filter 2 is a plain state-variable filter: no passband loss and its
         * own damping per resonance (read: noise through filter 2 alone, modes 5 and 7 alike, fits within 0.03-0.11
         * dB) */
        static const float K2A[14][2] = {{0, 1.401f}, {10, 1.318f}, {20, 1.236f}, {30, 1.154f}, {40, 1.039f}, {50, 0.899f},
            {60, 0.759f}, {70, 0.630f}, {80, 0.548f}, {90, 0.466f}, {100, 0.384f}, {110, 0.325f}, {120, 0.274f}, {127, 0.236f}};
        float rr = sy_clampf(r2, 0.f, 127.f), kk;
        if (vc[vi].k2r != rr) {     /* (per resonance) */
            int i = 0;
            while (i < 12 && rr > K2A[i + 1][0]) i++;
            kk = K2A[i][1] + (rr - K2A[i][0]) / (K2A[i + 1][0] - K2A[i][0]) * (K2A[i + 1][1] - K2A[i][1]);
            vc[vi].k2r = rr; vc[vi].k2k = kk;
            vc[vi].k2bp = BP0 * FDB2LIN(-0.00f) * kk * (1.f / 1.401f);   /* (the band pass's peak stays at reso
                                     * 0's: BPC[0]) */
        }
        kk = vc[vi].k2k;
        b->k2 = kk;
        b->h2e = 1.f / (1.f + kk * b->g2e + b->g2e * b->g2e);
        b->pg2 = 1.f;
        b->bp2 = vc[vi].k2bp;
    }
    {
        /* the analog modes are one transistor-ladder-like loop (read with a multitone at -50 dBFS, a
         * sine over the level and the free oscillation): four one-poles y += a (u - y), u = S(gin x - k y4)
         * with the last sample's y4; mode 5 takes stage 2, mode 7 stage 4. Per cutoff step (50-127, every 10):
         * the stages' corner, k = k0 + k1 x resonance, the gain (dB); below 50 the corner falls by 1.685 per
         * 10 steps */
        const float (*LC)[4] = LAD_LC;
        float cc = sy_clampf(c1, 0.f, 127.f), rr = sy_clampf(r1, 0.f, 127.f), fc, k0, k1, gd, t;
        if (cc < 50.f) {
            fc = LC[0][0] * FEXP2((cc - 50.f) * 0.1f * 0.75267f);    /* (log2 1.685) */
            k0 = LC[0][1]; k1 = LC[0][2]; gd = LC[0][3];
        } else {
            int i = cc >= 120.f ? 7 : (int)((cc - 50.f) * 0.1f);
            t = (cc - (50.f + 10.f * (float)i)) / (i == 7 ? 7.f : 10.f);
            fc = FEXP2(LAD_LC_L2[i] + t * (LAD_LC_L2[i + 1] - LAD_LC_L2[i]));   /* (log2 of the corners: once) */
            k0 = LC[i][1] + t * (LC[i + 1][1] - LC[i][1]);
            k1 = LC[i][2] + t * (LC[i + 1][2] - LC[i][2]);
            gd = LC[i][3] + t * (LC[i + 1][3] - LC[i][3]);
        }
#if LAD_FCOR
        {   /* the loop's resonance sits lower than the stages' corner gives, the most at low cutoffs (read on
             * the free oscillation and the hiss's peak, resonance 112: +174 cents at cutoff 0 to +24 at 64, -70 from 112; 8 cents less up to
             * resonance 100, 30 more at 127) */
            static const float FC_C[17] = {174.f, 150.f, 121.f, 97.f, 73.f, 54.f, 40.f, 32.f, 24.f, 24.f, 25.f, 5.f,
                                           -25.f, -35.f, -40.f, -40.f, -40.f};
            float x = cc * 0.125f, f;
            int q = (int)x;
            if (q >= 16) f = FC_C[16];
            else f = FC_C[q] + (x - (float)q) * (FC_C[q + 1] - FC_C[q]);
            f += rr <= 100.f ? -8.f : rr <= 112.f ? -8.f + 8.f / 12.f * (rr - 100.f) : 2.f * (rr - 112.f);   /* (by the resonance) */
            fc *= FEXP2(-f * sy_minf(rr * (1.f / LAD_FCOR_R), 1.f) * (1.f / 1200.f));
        }
#endif
#ifdef HYB_TUNE
        if (b->m1 == 5 && getenv("L5F")) fc *= (float)atof(getenv("L5F"));
#endif
        b->lad_a = 1.f - FEXP2(-6.2831853f * 1.44269504f * sy_minf(fc * s->ifs, 0.45f));
        {   /* mode 7's own noise falls with the cutoff (read without input, osc volume 0, the output's rms:
             * cutoff 30 / 50 / 70 / 100 / 127: -81 / -93 / -96 / -104 / -114 dBFS; silent at 14): dB re the
             * injection's full level */
            static const float NZC[7] = {14.f, 30.f, 50.f, 70.f, 100.f, 127.f, 128.f};
            static const float NZD[7] = {-90.f, -2.f, -10.f, -9.f, -14.f, -22.f, -22.f};
            int q = 0;
            float c = sy_clampf(cc, 14.f, 127.f);
            while (q < 5 && c > NZC[q + 1]) q++;
            b->lad_nz = FDB2LIN(NZD[q] + (c - NZC[q]) / (NZC[q + 1] - NZC[q]) * (NZD[q + 1] - NZD[q]));
        }
        b->lad_k = k0 + k1 * rr;
        /* mode 7 saturates its stages less at a high resonance (read on the test bank's driven acid lines,
         * resonance 108-116, osc volume 84-110: 0.79 -> 0.64 dB, their level -0.8 -> -0.4 dB; the trance patches at
         * resonance 40-70 read best at the base value, and mode 5's bass too; the change goes from 80 to 108) */
        b->lad_ss = LAD_STS_B;
        if (b->m1 == 7) b->lad_ss += (LAD_STS_H - LAD_STS_B) * sy_clampf((rr - LAD_STS_R0) / (LAD_STS_R1 - LAD_STS_R0), 0.f, 1.f);
        if (rr > 96.f)              /* above ~100 the feedback grows fast (the passband: -20 dB at 112, -30 at 127) */
            b->lad_k += 2.5f * (fmemo_exp2(vc[vi].mm[MM_LKX], (rr - 112.f) * (1.44269504f / 7.05f)) - 0.1034f);
        {   /* the osc volume above 64 drives the loop, the output falling by a third of it (a sine's
             * harmonics and level over the volume) */
            float dd = LAD_DRV / 63.f * sy_clampf(E(OSC_VOL) - 64.f, 0.f, 63.f);
            b->lad_gin = LAD_GIN * fmemo_db2lin(vc[vi].mm[MM_LGIN], dd);
            gd -= LAD_DDF * dd;
        }
        {   /* a correction of the output by cutoff and osc volume (read: a held saw, key 45, resonance 0
             * and 50 averaged, against the reference; the drive left it up to 1.3 dB loud at cutoff 90-110 and
             * 1.6 dB quiet at 127) */
            static const float CX[6] = {30.f, 50.f, 70.f, 90.f, 110.f, 127.f}, VX[4] = {64.f, 90.f, 110.f, 127.f};
            static const float GC[6][4] = {{-0.01f, -0.27f, -0.45f, -0.41f}, {0.27f, -0.16f, -0.32f, -0.19f},
                {0.22f, -0.31f, -0.45f, -0.43f}, {0.07f, -0.59f, -0.91f, -0.95f}, {0.11f, -0.71f, -1.19f, -1.28f},
                {0.84f, 1.03f, 1.31f, 1.48f}};
            float vv = sy_clampf(E(OSC_VOL), 64.f, 127.f), tc, tv, g0, g1;
            int ic = 0, iv = 0;
            cc = sy_clampf(cc, 30.f, 127.f);
            while (ic < 4 && cc > CX[ic + 1]) ic++;
            while (iv < 2 && vv > VX[iv + 1]) iv++;
            tc = (cc - CX[ic]) / (CX[ic + 1] - CX[ic]); tv = (vv - VX[iv]) / (VX[iv + 1] - VX[iv]);
            g0 = GC[ic][iv] + tv * (GC[ic][iv + 1] - GC[ic][iv]);
            g1 = GC[ic + 1][iv] + tv * (GC[ic + 1][iv + 1] - GC[ic + 1][iv]);
            gd += LAD_GCOR * (g0 + tc * (g1 - g0));
        }
        b->lad_gout = LAD_GOUT * FDB2LIN(gd + 1.70f);
        if (b->m1 == 5)             /* the 2-pole tap reads 0.85 dB lower at resonance 0, less up to ~100
                                     * (saws at osc volume 40 and 64, cutoff 20-127, resonance 0 / 30 / 80) */
            b->lad_gout *= fmemo_db2lin(vc[vi].mm[MM_LG5], sy_minf(0.f, -0.85f + 0.0085f * rr));
#ifdef HYB_TUNE
        if (b->m1 == 5 && getenv("L5G")) b->lad_gout *= FDB2LIN((float)atof(getenv("L5G")));
        if (b->m1 == 5 && getenv("L5K")) b->lad_k *= (float)atof(getenv("L5K"));
#endif
        {
            float tc = sy_clampf((rr - LAD_CR0) / (LAD_CR1 - LAD_CR0), 0.f, 1.f);
            b->lad_c = 1.f + (LAD_C - 1.f) * tc;
            b->lad_gout *= FDB2LIN(-LAD_CD * tc * LAD_DRV / 63.f * sy_clampf(E(OSC_VOL) - 64.f, 0.f, 63.f));
        }
        b->lad_ss1 = b->lad_ss;
#if LAD_Q_ON
        if (b->m1 == 7) {           /* the quench: see LAD_Q_ON */
            const float q0 = LAD_Q_R0, q1 = LAD_Q_R1, qm = LAD_Q_LCM, qs = LAD_Q_SS1;
            float tq = sy_clampf((rr - q0) / (q1 - q0), 0.f, 1.f);
            if (tq > 0.f) {         /* (below LAD_Q_R0 the loop as before, bit for bit) */
                b->lad_c *= 1.f + (qm - 1.f) * tq;
                /* (a line over the osc volume: within 0.15 dB of 0.15 x the drive in dB) */
                float sv = qs * (1.f + (LAD_Q_DG - 1.f) / 63.f * sy_clampf(E(OSC_VOL) - 64.f, 0.f, 63.f));
                b->lad_ss1 += (sv - b->lad_ss1) * tq;
            }
        }
#endif
        if (LAD_HIR && b->m1 == 7 && rr > 112.f) {   /* above resonance 112 the passband read 1-2 dB loud
                                     * without drive (saw, cutoff 60: +1.9 dB at 116, +1.5 at 120, +0.8 at 127),
                                     * half of it at osc volume 84 and none from 110 */
            float c = rr < 116.f ? 1.9f * (rr - 112.f) / 4.f : rr < 120.f ? 1.9f - 0.1f * (rr - 116.f) : 1.5f - 0.1f * (rr - 120.f);
            b->lad_gout *= FDB2LIN(-c * sy_clampf(1.f - (E(OSC_VOL) - 64.f) / 46.f, 0.f, 1.f));
        }
    }
}

/* the saturation types 5, 6 and 8-14: their coefficients per drive (voice_block; per block while the type is on: in
 * the flash, VB_FLASH); b->sat_a is 0 and b->sat_b the general height on entry */
VB_FN void sat_set(const sy_synth *s, sy_vblk_t *b, int vi, int t, float drv, float dv, float key)
{
#ifdef CLN_SAT5_OWN
    if (t == 5) b->sat_g5 = 1.66f * FEXP2(6.f * drv);   /* (a measurement build: the own former law, fit_sat5.py) */
#endif
#ifndef CLN_SAT5_OWN
    if (t == 5) {   /* CLEANER: the clip's slope and limit per drive, measured on the development engine as a black box (a ramp
                     * through the static path: tools/measure/meas_sat5.py, fit_sat5.py: sy_cln_sat5.h), linear
                     * between the drives 64, 66, .., 126, 127 (slope) and 64, 72, .., 120, 127 (limit) */
        float q = sy_clampf(dv - 64.f, 0.f, 63.f) * 0.5f, f, q8 = sy_clampf(dv - 64.f, 0.f, 63.f) * 0.125f, f8;
        int k = (int)q, k8 = (int)q8;
        if (k >= 31) { k = 31; f = (dv - 126.f); } else f = q - (float)k;
        if (k8 >= 7) { k8 = 7; f8 = (dv - 120.f) / 7.f; } else f8 = q8 - (float)k8;
        f = sy_clampf(f, 0.f, 1.f); f8 = sy_clampf(f8, 0.f, 1.f);
        b->sat_g5 = SAT5_GF * (CLN_SAT5_G[k] + f * (CLN_SAT5_G[k + 1] - CLN_SAT5_G[k]));
        b->sat_b = SAT_TRIM * (CLN_SAT5_B[k8] + f8 * (CLN_SAT5_B[k8 + 1] - CLN_SAT5_B[k8]));
    }
#endif
    if (t == 6) {   /* the shaper's period and height per drive 64, 68 .. 124, 127 (read: a sine through the
                     * static path, fitted as A sin(2 pi k x); its period is piecewise linear between drives of 8) */
        static const float K6[17] = {3.4138f, 3.9185f, 4.4232f, 5.0772f, 5.7317f, 6.5784f, 7.4257f, 8.5232f, 9.6214f,
            11.0433f, 12.4663f, 14.3113f, 16.1517f, 18.5400f, 20.9318f, 24.0247f, 26.3452f};
        static const float A6[17] = {0.04778f, 0.04703f, 0.04628f, 0.04553f, 0.04478f, 0.04403f, 0.04328f, 0.04252f,
            0.04177f, 0.04100f, 0.04025f, 0.03947f, 0.03870f, 0.03793f, 0.03713f, 0.03633f, 0.03569f};   /* (before the
                     * part of the stage's output that falls with the drive: read back on the engine, iterated once) */
        float q = (dv - 64.f) * 0.25f, f;
        int k = (int)q;
        if (k >= 15) { k = 15; f = (dv - 124.f) / 3.f; } else f = q - (float)k;
        b->shaper_k = K6[k] + f * (K6[k + 1] - K6[k]);
        b->sat_b = (A6[k] + f * (A6[k + 1] - A6[k])) * (0.081f * 1.036f * SAT_TRIM / 0.04957f);   /* (the stage's own
                     * output scale, read at drive 64) */
    }
    switch (t) {
    case 8: {                                   /* a quantiser, no hold (read with noise in: the runs are
                                                 * single samples), its step per drive from 64 (read with a ramp:
                                                 * 0.0015 at the output up to 64, 0.085 at 127) */
        static const float SAT8_Q[64] = {
            0.002288f, 0.002440f, 0.002615f, 0.002816f, 0.003051f, 0.003328f, 0.003661f, 0.004067f,
            0.004576f, 0.004881f, 0.005230f, 0.005632f, 0.006101f, 0.006656f, 0.007322f, 0.008135f,
            0.009152f, 0.009762f, 0.010460f, 0.011264f, 0.012203f, 0.013313f, 0.014644f, 0.016271f,
            0.018305f, 0.019101f, 0.019969f, 0.020920f, 0.021966f, 0.023122f, 0.024407f, 0.025843f,
            0.027458f, 0.028344f, 0.029288f, 0.030298f, 0.031380f, 0.032543f, 0.033794f, 0.035146f,
            0.036610f, 0.038202f, 0.039938f, 0.041841f, 0.043933f, 0.046245f, 0.048815f, 0.051686f,
            0.054916f, 0.056688f, 0.058579f, 0.060597f, 0.062762f, 0.065088f, 0.067589f, 0.070294f,
            0.073220f, 0.078101f, 0.083680f, 0.090117f, 0.097627f, 0.106502f, 0.117152f, 0.130169f};
        b->sat_a = SAT8_Q[(int)sy_clampf(dv - 64.f + 0.5f, 0.f, 63.f)];    /* (in a voice the stage's own gains
                                                 * (SAT_VPRE, sy_voice.c) make it 3.8 % coarser) */
        b->sat_c = 1.f;
        break;
    }
    case 9: {                                   /* a sample hold, its length per drive, fractional (read in
                                                 * a voice, the noise oscillator in, keys 24-96 alike: the mean run
                                                 * 1 / 4.42 / 7.15 / 11.5 / 18.7 / 30.2 / 48.8 / 79.0 / 120 at
                                                 * 64 / 72 / ... / 120 / 127; at 46,875 Hz. The input path without
                                                 * a voice holds less: 92 at 127) */
        const float *H = SAT_H9_L2;             /* (log2 of the lengths: once) */
        float q = (dv - 64.f) / 8.f, hh;
        int qi = q >= 7.f ? 7 : (int)q;
        float w = qi == 7 ? (dv - 120.f) / 7.f : q - (float)qi;
        hh = fmemo_exp2(vc[vi].mm[MM_S0], H[qi] + w * (H[qi + 1] - H[qi]));
        b->sat_a = sy_maxf(1.f, hh * s->fs / REF_FS);
        break;
    }
    case 10: {                                  /* a sample hold, its length per drive at key 60, x 2 per
                                                 * octave below (/ 2 above), at most 128; short holds shorter still
                                                 * (read in a voice, the noise oscillator in, keys 24-96: the mean
                                                 * run; at 46,875 Hz) */
        const float *H = SAT_H10_L2;            /* (log2 of the lengths: once) */
        static const float SH[7][2] = {{0.f, 1.f}, {2.75f, 1.f}, {2.885f, 2.06f}, {3.575f, 3.18f}, {3.775f, 3.55f},
            {4.68f, 4.69f}, {5.f, 5.f}};
        float q = (dv - 64.f) / 8.f, hh;
        int qi = q >= 7.f ? 7 : (int)q;
        float w = qi == 7 ? (dv - 120.f) / 7.f : q - (float)qi;
        hh = fmemo_exp2(vc[vi].mm[MM_S0], H[qi] + w * (H[qi + 1] - H[qi]));
        hh = sy_minf(128.f, hh * fmemo_exp2(vc[vi].mm[MM_S1], (60.f - key) / 12.f));
        if (hh < 5.f) {
            int j = 0;
            while (j < 5 && hh > SH[j + 1][0]) j++;
            hh = SH[j][1] + (hh - SH[j][0]) / (SH[j + 1][0] - SH[j][0]) * (SH[j + 1][1] - SH[j][1]);
        }
        b->sat_a = sy_maxf(1.f, hh * s->fs / REF_FS);
        break;
    }
    case 11: case 12: case 13: case 14: {
        /* one table for all four (fitted one-pole on a saw, v = drive 64..127 in steps of 3):
         * 11 low pass at T(v), 13 high pass at T(192 - v); 12 and 14 alike but x 2^((key - 60) / 24) */
        const float *T = SAT_T;
        float u = (t <= 12 ? dv : 192.f - dv) - 64.f, hz;
        int k;
        u = sy_clampf(u, 0.f, 64.f) / 3.f;
        k = u >= 20.f ? 20 : (int)u;
        hz = T[k] * fmemo_exp2(vc[vi].mm[MM_S0], (u - (float)k) * SAT_T_L2R[k]);   /* (log2 of the ratio: once) */
        if (t == 12 || t == 14) hz *= fmemo_exp2(vc[vi].mm[MM_S1], (key - 60.f) / 24.f);
        if (t >= 13) {              /* the high pass's own corners (read: noise, a bilinear fit within 0.01 dB,
                                     * every 3 steps of the drive 64-127) */
            const float *HK = SAT_HK;
            float uh = sy_clampf(dv - 64.f, 0.f, 63.f) / 3.f, kk;
            int kh;
            if (t == 14) uh += (key - 60.f) / 12.f;  /* (the key moves along the drive: an octave per step) */
            uh = sy_maxf(uh, 0.f);
            kh = uh >= 20.f ? 20 : (int)uh;          /* (beyond 127 the last step's ratio continues) */
            kk = HK[kh] * fmemo_exp2(vc[vi].mm[MM_S2], (uh - (float)kh) * SAT_HK_L2R[kh]);   /* (log2 of the ratio: once) */
            if (s->fs != REF_FS && kk < 1e3f)        /* (the same corner at another rate) */
            {
                float x = kk > 1.f ? 1.f / kk : kk, at = x * (0.9998660f + x * x * (-0.3302995f + x * x * (0.1801410f
                          + x * x * (-0.0851330f + x * x * 0.0208351f))));   /* (atan on 0..1, ~1e-5) */
                if (kk > 1.f) at = 1.5707963f - at;
                kk = FTANPI(sy_minf(0.4999f, at * (1.f / 3.14159265f) * REF_FS * s->ifs));
            }
            b->sat_a = 1.f / (1.f + kk);
            b->sat_b = (1.f - kk) / (1.f + kk);
        } else
        {                           /* (onepole_a, its exp2 per argument) */
            float x = SY_2PI * sy_clampf(hz, 19.7f, 0.45f * s->fs) * s->ifs;
            b->sat_a = x >= 3.f ? 1.f : 1.f - fmemo_exp2(vc[vi].mm[MM_S2], -x * 1.44269504f);
        }
        break;
    }
    default: break;
    }
    (void)drv;
}

SY_CTL static void voice_block(sy_synth *s, sy_voice_t *v, sy_vblk_t *b, float *m, int newest)
{
    uint8_t touched[12];
    int nt;
    int atk0;
    int vi = v == &s->stat ? SY_MAX_VOICES : SY_VI(s, v);   /* the voice's cache (vc) */
    float dt = s->bdt, key, p1, p2, semi1, hz1, hz2, det2c, fe, ae, lfo1, lfo2, lfo1p, lfo3p, d, x, cents1 = 0.f, cents2 = 0.f;
    float shape1, shape2, pw1, pw2, fmamt, c1, c2, vol, pan, plfo, uspr, velf, amp, gain_db = 0.f, fgain = 1.f;
    float hp1, hp2, hg1, hg2;           /* the oscillators' pitch (hi_pitch) and read level (hi_gain) */
    float dww = 0.f;                    /* the detune's wander (DET_WANDER), of the nominal amount */
    int i, vel = v->vel, routing, uni_n = v->uni_n;
#ifdef SY_UNI_CAP
    int uramp;
#endif
    nt = matrix(s, v, m, touched);      /* (for the envelopes' and LFOs' own parameters: last block's sources; m is all
                                         * 0 on entry, and again on return: only the slots touched are cleared) */

    /* portamento: exponential in pitch, the remaining interval floored at 0.95 semitone, so the last semitone
     * goes at a constant speed (mod.json portamento: within 2-5 cents; ASSUMPTION: the floor is absolute, the
     * glides measured were 12 semitones), per block */
    if (v->glide != v->target) SY_FAR(porta_step)(s, v, m, dt);   /* (S1: in the flash, while a glide runs) */
    key = v->glide + (E(TRANSPOSE) - 64.f);

    /* envelopes */
    atk0 = v->aenv.stage == 1;
    if (atk0) v->aenv.alen = 0.f;
    env_step(s, &v->aenv, E(AE_A), E(AE_D), sy_tab(s, T_SUS, E(AE_S)), E(AE_ST), E(AE_R), 1, vc[vi].mm[MM_AENV]);
    env_step(s, &v->fenv, E(FE_A), E(FE_D), sy_clampf(E(FE_S), 0.f, 127.f) / 127.f, E(FE_ST), E(FE_R), 0, vc[vi].mm[MM_FENV]);
    if (!v->aenv.stage) { v->on = 0; v->gate = 0; }
    ae = v->aenv.lvl;
    fe = v->fenv.lvl;
    v->t_on += dt;

#ifdef CLN_LFO_TAB
    if (v->lfo_pend) SY_FAR(lfo_pend_start)(s, v, key);   /* (S1: a voice's first block, in the flash) */
#endif
    /* LFOs: per voice (poly) or the part's (mono) */
    {
        static const int RATE[3] = {L1_RATE, L2_RATE, B_L3_RATE}, SHAPE[3] = {L1_SHAPE, L2_SHAPE, B_L3_SHAPE};
        static const int MODE[3] = {L1_MODE, L2_MODE, B_L3_MODE}, KF[3] = {L1_KF, L2_KF, B_L3_KF};
        static const int CLK[3] = {B_CLK_L1, B_CLK_L2, B_CLK_L3}, ENV[3] = {L1_ENV, L2_ENV, -1};
        static const int SYM[3] = {L1_SYM, L2_SYM, -1};
        float out[3], outp[3];
        int pass;
        for (i = 0; i < 3; i++) {
            int envm = ENV[i] >= 0 && s->patch[ENV[i]];
            float sym = SYM[i] >= 0 ? sy_clampf(0.5f + (E(SYM[i]) - 64.f) / 126.f, 0.f, 1.f) : 0.5f;
            sy_lfo_t *l = s->patch[MODE[i]] ? &s->glfo[i] : &v->lfo[i];
            /* (envelope mode: the one cycle runs at twice the rate: mod.json lfo1_envmode) */
            int trg = i < 2 ? s->patch[i ? L2_TRIG : L1_TRIG] : 0, ck = s->patch[CLK[i]];
            int clkd = ck >= 1 && ck <= 19 && !trg && !envm;
            float lh = l == &v->lfo[i] || clkd ? SY_FAR(lfo_hz)(s, E(RATE[i]), ck, s->patch[KF[i]], key) : 0.f;   /* (once) */
            float lhz = l == &v->lfo[i] ? lh * (envm ? 2.f : 1.f) : 0.f;
            for (pass = 0; pass < 2; pass++) {
                /* two readings, at the block's start (the oscillators' pitch) and after the advance (all
                 * else): with key trigger the pitch follows one block behind the cutoff and the pan */
                float ph = l->ph;       /* (read here, not written: a mono LFO is the part's, read by both cores) */
                if (clkd) {
                    /* a clocked LFO without key trigger runs half a cycle against the free-running phase */
                    ph = ph + 0.445f - LFO_CLK_LAG * s->bdt * lh;
                    ph -= (float)sy_floori(ph);   /* (0.445: re-read on the voice, clocks 5-17; LFO_CLK_LAG
                                         * blocks: re-read on the gate's edges, clocks 4-17) */
                    if (s->patch[B_ARP] && s->arp_on) {     /* under the arp the phase counts from its start */
                        float q = ARP_LFO_P0 + s->arp_lt * lh;
                        ph = q - (float)sy_floori(q);
                    }
                }
                x = lfo_shape(l, ph, s->patch[SHAPE[i]], sym);   /* (one call site: one inlined copy) */
                if (envm) x = l->done ? 0.f : x + 1.f;  /* envelope mode: one unipolar cycle */
                if (pass == 0) {
                    outp[i] = x;
                    if (l == &v->lfo[i]) lfo_advance(l, lhz, dt, envm);
                }
            }
            v->lfo[i].out = x;
            if (l != &v->lfo[i]) v->lfo[i].ph = l->ph;
            out[i] = x;
        }
        lfo1 = out[0]; lfo2 = out[1];
        lfo1p = outp[0]; lfo3p = outp[2];
    }
    /* the matrix again with this block's envelopes and LFOs: its sources act without a block's delay
     * (read on the cutoff from the filter envelope and both LFOs, against their direct paths) */
    for (i = 0; i < nt; i++) m[touched[i]] = 0.f;
    nt = matrix(s, v, m, touched);
    (void)newest;
    if (!v->started && m[B_INIT_PH] != 0.f) SY_FAR(init_phase)(s, v, m);   /* (S1: in the flash) */
    if (v->age == s->ages && v != &s->stat) SY_FAR(fx_take)(s, v, m);   /* (the effects' offsets: fx_take) */

    /* LFO 3: unipolar amount (cents), fade-in, destination: 0 and 1 = osc 1 pitch (measured:
     * 0 moves osc 1 as 1 does, 2-6 leave osc 1 alone); ASSUMPTION 2 = osc 2, 3-6 none */
    {
        float amt = sy_tab(s, T_L3_CENTS, E(B_L3_AMT)), fd = bc.fd, fr = bc.fr, w = 1.f;
        int dst = s->patch[B_L3_DEST];
        if (s->patch[B_L3_FADE]) w = sy_clampf((v->t_on - fd) / sy_maxf(fr, 1e-3f), 0.f, 1.f);
        x = amt * w * lfo3p;
        if (dst <= 1) cents1 += x;
        else if (dst == 2) cents2 += x;
    }

    /* pitch (semitones): keyfollow around 60, semitone + its modulations clamped at +-48, then the bend */
    d = E(L1_O1) - 64.f;
    cents1 += (d < 0.f ? -1.f : 1.f) * sy_tab(s, T_LFO_CENTS, sy_absf(d)) * lfo1p;
    d = E(L1_O2) - 64.f;
    cents2 += (d < 0.f ? -1.f : 1.f) * sy_tab(s, T_LFO_CENTS, sy_absf(d)) * lfo1p;
    {
        /* the direct bend is a curve in four straight pieces (measured, range 2: 0.127 / 0.311 /
         * 0.627 of the range at a quarter / half / three quarters, alike both ways); ASSUMPTION: the same shape for
         * every range */
        static const float BC[5] = {0.f, 0.1266f, 0.3112f, 0.6267f, 1.f};
        float ab = sy_absf(s->bend) * 4.f, bcv, bend;
        int ib = (int)ab;
        if (ib > 3) ib = 3;
        bcv = BC[ib] + (ab - (float)ib) * (BC[ib + 1] - BC[ib]);
        bend = s->bend >= 0.f ? bcv * (float)(s->patch[B_BEND_UP] - 64) : bcv * (float)(s->patch[B_BEND_DN] - 64);
        float uni = 0.f;
        if (uni_n > 1)                      /* wider with more voices (mod.json unison: 4 voices at 48 reach -6.4) */
        {                                   /* (more than two: -2.45 .. +0.5 x the two-voice detune, mod.json) */
            /* the voices sit evenly from -D to +D; an even count leaves the centre free (4: -1 -1/2 1/2 1);
             * D narrows up the keyboard (x 2^((60 - key)/52.6)) */
            float d2 = sy_tab(s, T_UNI_DET, sy_clampf(E(UNI_DET), 0.f, 127.f)) * memo_exp2(vc[vi].mm[MM_UNI], (60.f - key) * (1.f / 52.6f));
            if (UNI_PAIRS) uni = d2 * uni_x(uni_n, v->uni_i);
            else {
                static const uint8_t KOFF[9] = {0, 10, 5, 15, 9, 3, 13, 19, 17};
                int j = v->uni_i, m = uni_n, dc;
                if (!(uni_n & 1)) { m = uni_n + 1; if (j >= uni_n / 2) j++; }
                dc = j - (m - 1) / 2;
                if (dc < 0) dc = -dc;
                if (!(uni_n & 1)) dc--;
                uni = d2 * (2.f * (float)j / (float)(m - 1) - 1.f);
                if (!v->iph) uni -= 0.2488f * (float)KOFF[dc > 8 ? 8 : dc] * memo_exp2(vc[vi].mm[MM_UNK], (96.f - key) * (1.f / 52.6f));
            }
        }
#if DET_WANDER
        if (E(O2_DET) < DW_OFF) dww = s->dw_w[v != s->v];   /* (the wander: dw_tick's; the part's first voice its own) */
#endif
        det2c = sy_tab(s, T_DET2, sy_clampf(E(O2_DET), 0.f, 127.f))
              * ((v->iph ? 0.8135f + 0.00155f * (float)v->iph
                                     : 0.8f + 0.2f * v->du) + dww)   /* DET2_K: the tuning step scales the
                                         * detune (read B 35 = 1 / 32 / 64 / 127: x 0.815 / 0.863 / 0.912 / 1.010 at every
                                         * detune; free running x (0.8 + 0.2 u), detune 8-127, keys 48 and 72) */
              * memo_exp2(vc[vi].mm[MM_DET2], (60.f - key) * (key < 60.f ? 1.f / 51.1f : 1.f / 52.2f));   /* the detune narrows up the keyboard */
        semi1 = ji_semi(sy_clampf(E(O1_SEMI) - 64.f, -48.f, 48.f));   /* the transposition (value and
                                         * modulation together) runs on a just scale: 17/16, 9/8, 6/5, 5/4, 4/3, 7/5,
                                         * 3/2, 8/5, 5/3, 7/4, 15/8 (read per semitone on both oscillators) */
        p1 = 60.f + (key - 60.f) * (E(O1_KF) - 64.f) / 32.f + semi1 + bend + (cents1 + uni) * 0.01f;
        p2 = 60.f + (key - 60.f) * (E(O2_KF) - 64.f) / 32.f
           + ji_semi(sy_clampf(E(O2_SEMI) - 64.f, -48.f, 48.f))
           + hyb_tab(HYB_O2FEP, sy_clampf(E(O2_FENV_PITCH), 0.f, 127.f)) * fe   /* the measured law */ + bend
           + (cents2 + uni + det2c) * 0.01f;
        {   /* (per pitch, again when it changed: the frequency, hi_pitch, hi_gain; osc 1's clk_rate too) */
            float st = pitch_st(s, v, key);
            int stc = vc[vi].st != st;
            vc[vi].st = st;
            if (stc || vc[vi].p1 != p1) {
                vc[vi].p1 = p1;
                hz1 = vc[vi].hz1 = pitch_hz(p1, st);
                vc[vi].hp1 = hi_pitch(hz1);
                vc[vi].hg1 = hi_gain(hz1, vc[vi].hp1);
                vc[vi].rate = CEXP2(1.3f * CLOG2(sy_maxf(hz1, 1.f) * (1.f / 27.5f))) * REF_FS * s->ifs;
            }
            if (stc || vc[vi].p2 != p2) {
                vc[vi].p2 = p2;
                hz2 = vc[vi].hz2 = pitch_hz(p2, st);
                vc[vi].hp2 = hi_pitch(hz2);
                vc[vi].hg2 = hi_gain(hz2, vc[vi].hp2);
            }
            hz1 = vc[vi].hz1; hp1 = vc[vi].hp1; hg1 = vc[vi].hg1;
            hz2 = vc[vi].hz2; hp2 = vc[vi].hp2; hg2 = vc[vi].hg2;
        }
        if (s->patch[B_O3_MODE] >= 2) {
            int m3 = s->patch[B_O3_MODE], w = m3 == 2 ? -1 : m3 == 3 ? -2 : m3 == 4 ? 0 : m3 == 5 ? 1 : wave_of(m3 - 4, 1);
            float p3 = key + ji_semi((float)s->patch[B_O3_SEMI] + m[B_O3_SEMI] - 64.f) + bend   /* the just
                                         * scale as osc 1 and 2; the matrix's offsets 97-99 (MEASURED: per voice, the
                                         * semitone not in whole steps) */
                     + (uni - sy_tab(s, T_DET3, (float)s->patch[B_O3_DET] + m[B_O3_DET]) * (1.f + dww) * memo_exp2(vc[vi].mm[MM_O3D], (60.f - key) * (key < 60.f ? 1.f / 50.4f : 1.f / 53.1f))) * 0.01f;
            float hz3 = pitch_hz(p3, vc[vi].st);
            if (w == -1) osc_setup(s, &b->o3, &vc[vi].o[2], 64.f, 0.f, 0, 2, hz3);
            else if (w == -2) osc_setup(s, &b->o3, &vc[vi].o[2], 127.f, 0.f, 0, 2, hz3);
            else osc_setup(s, &b->o3, &vc[vi].o[2], 0.f, 0.f, w, 2, hz3);
            b->o3_on = 1;
            b->g3 = ((float)s->patch[B_O3_VOL] + m[B_O3_VOL]) / 127.f * (m3 == 5 ? O3_G * 0.818f : O3_G);   /* rest.json osc3: +4.5 dB, the
                                                                                         * triangle 1.75 dB under the sine */
        } else {
            b->o3_on = 0;
            b->g3 = 0.f;
        }
    }

    /* the oscillators' shape, pulse width, FM */
    shape1 = E(O1_SHAPE) + vel_lin(E(B_V_O1SH), vel);   /* (the velocity amounts with the matrix's offsets: 72-81) */
    shape2 = E(O2_SHAPE) + vel_lin(E(B_V_O2SH), vel);
    d = E(L2_SHAPEAMT) - 64.f;                      /* LFO 2 -> shape: one shape step per amount step */
    shape1 += d * lfo2;
    shape2 += d * lfo2;
    d = E(L1_PW) - 64.f;                            /* LFO 1 -> pulse width: two pw steps per amount step */
    {   /* (velocity: 2 steps, open.json; MEASURED (1.00): on osc 2's pulse width too, as on osc 1's) */
        const float vpw = 2.f * vel_bip(E(B_V_PW), vel);
        pw1 = E(O1_PW) + 2.f * d * lfo1 + vpw;
        pw2 = E(O2_PW) + 2.f * d * lfo1 + vpw;
    }
    osc_setup(s, &b->o1, &vc[vi].o[0], shape1, pw1, wave_of((int)(E(O1_WAVE) + 0.5f), 1), 0, hz1);
    osc_setup(s, &b->o2, &vc[vi].o[1], shape2, pw2, wave_of((int)(E(O2_WAVE) + 0.5f), 2), 1, hz2);
    if (s->patch[B_O3_MODE] == 1) {    /* osc 3 mode 1 = a second osc 2 with the detune mirrored (osc 2 at
                                         * +d cents, this one at -d), 0.6 dB above osc 2; its own phase */
        osc_setup(s, &b->o3, &vc[vi].o[2], shape2, pw2, wave_of((int)(E(O2_WAVE) + 0.5f), 2), 1, hz2 * memo_exp2(vc[vi].mm[MM_O3M], -2.f * det2c / 1200.f));
        b->o3_on = 1;
        b->g3 = b->g2 * 1.07f;          /* (1.07: measured against osc 2 alone, in unison) */
    }
    fmamt = E(O2_FM) + (E(L2_FM) - 64.f) * lfo2 + VFM_K * vel_lin(E(B_V_FM), vel);
    fmamt = sy_clampf(fmamt, 0.f, 127.f);
    b->fm_mode = s->patch[B_FM_MODE] > 4 ? 4 : s->patch[B_FM_MODE];
    b->fm_saw = shape1 >= 64.f;                   /* mode 2's modulator (sy_voice.c) */
    if (b->fm_mode > 3) b->fm_i = sy_tab(s, T_FM_I, fmamt);
    else
    {   /* the FM index (osc 2's deviation re its own frequency) is the static amount's index plus the filter
         * envelope's, added as indices, not as amounts, and not limited to the static range (MEASURED on black-box
         * renders, tools/measure/fm_index.py: FM_IA at amount 0-127, FM_IB at envelope amount 64-127, the envelope
         * held at full). Envelope amounts below 64 add nothing. */
        /* modes 0-2: linear FM of osc 2, the deviation a share of osc 2's own frequency (measured, reset phases:
         * the share is the amount / 32 up to 32, then (amount / 32)^2). Mode 0 takes osc 1 unipolar ((1 + x) / 2:
         * osc 2 moves up by amount / 64 of itself, 1.125 / 1.25 / 1.5 at 8 / 16 / 32), modes 1 and 2 bipolar.
         * ASSUMPTION: mode 1 as mode 2 (it fits within ~5 dB per harmonic; its even harmonics are stronger) */
        float fb = sy_clampf(E(O2_FENV_FM), 64.f, 127.f);
        if (vc[vi].fa != fmamt) { vc[vi].fa = fmamt; vc[vi].ia = fm_law(FM_IA, fmamt); }
        if (vc[vi].fb != fb) { vc[vi].fb = fb; vc[vi].ib = fm_law(FM_IB, fb - 64.f); }
        b->fm_i = vc[vi].ia + vc[vi].ib * fe;
    }
    b->sync = s->patch[O2_SYNC] != 0;
    b->sync_p0 = SYNC_PH0 && b->sync ? (uint32_t)(sy_clampf(E(O2_DET), 0.f, 127.f) * 33554432.f) : 0u;

    /* the mix: balance, osc volume (a level up to 64, then drive), sub, noise, ring (not under the osc volume) */
    {
        /* the balance: each oscillator's gain read from its own lines (every 8 steps, two sines, a
         * symmetric cross-fade, -2 dB each at 64), piecewise linear between */
        static const float G1[17] = {1.f, 0.9661f, 0.9441f, 0.9226f, 0.8913f, 0.8710f, 0.8414f, 0.8128f, 0.7943f,
            0.6918f, 0.5957f, 0.4955f, 0.3936f, 0.2951f, 0.1972f, 0.0989f, 0.f};
        static const float G2[17] = {0.f, 0.1000f, 0.1995f, 0.2985f, 0.3981f, 0.4955f, 0.5957f, 0.6998f, 0.7943f,
            0.8222f, 0.8511f, 0.8710f, 0.9016f, 0.9226f, 0.9550f, 0.9772f, 1.f};
        float bal = sy_clampf(E(OSC_BAL), 0.f, 127.f), fb;
        float ov = sy_tab(s, T_OSCVOL, sy_clampf(E(OSC_VOL), 0.f, 127.f));   /* the matrix (destination 22) */
        if (VC6_GAIN && s->v6)   /* with 6 or more voices on (releases included; CLEANER: or by the load,
                                         * v6_on) the oscillators of every voice read 0.55 dB louder, and back below
                                         * 6; the noise does not (read: a sine or saw held while 1-24 more notes are added and released, release 0 and 84
                                         * alike; noise voices keep their level; mode 7 shows it compressed, +0.1) */
            ov *= 1.0654f;
        float subhz = hz1 * 0.5f;
        int ib = (int)(bal * 0.125f);
        if (ib > 15) ib = 15;
        fb = ib == 15 ? (bal - 120.f) * (1.f / 7.f) : bal * 0.125f - (float)ib;
        b->g1 = (G1[ib] + fb * (G1[ib + 1] - G1[ib])) * ov * hg1;
        b->dip = 0;
        if (key > 97.75f) {     /* high keys: a dip around 5 kHz on the oscillators (read with sines and saw lines
                                 * at keys 84-120 and the same pitches: 2.9 kHz -2.1, 4.2 kHz -4.6, 5.9 kHz -4.5, 8.4 kHz -2.1,
                                 * 12.5 kHz -0.7 dB at keys 108 and up), deepening from key 97.75 to 108 */
            float *dp = vc[vi].dp;      /* (per key: again only when it changed; the rate is set with the caches'
                                         * reset, sy_init) */
            if (vc[vi].dk != key) { vc[vi].dk = key; SY_FAR(dip_coef)(s, dp, key); }   /* (S1: in the flash, per key) */
            b->dip = 1;
            b->dp[0] = dp[0]; b->dp[1] = dp[1]; b->dp[2] = dp[2]; b->dp[3] = dp[3]; b->dp[4] = dp[4];
        }
        b->g2 = (G2[ib] + fb * (G2[ib + 1] - G2[ib])) * ov * hg2;
        b->g3 *= ov;
        /* the sub's and the noise's volume follow the ring modulator's law (the same curve, measured A/B:
         * -12.2 dB at 64, -35 dB at 8 re 127), not a straight line */
        b->sub_g = hyb_tab(HYB_RING_G, sy_clampf(E(SUB_VOL), 0.f, 127.f)) / HYB_RING_G[127] * SUB_G * ov;     /* +4.8 dB re the saw (square) */
        b->sub_tri = s->pv[SUB_SHAPE] >= 0.5f ? 1.f : 0.f;      /* 0 square, 1-127 triangle (no crossfade) */
        b->sub_inc = hz_inc(s, subhz);
        b->sub_dt = sy_minf(0.5f, subhz * 2.f / s->fs);
        b->ring_g = hyb_tab(HYB_RING_G, sy_clampf(E(RING_VOL), 0.f, 127.f)) * s->tables[T_SHAPE_G][0];   /* (sy_tab at 0) */
        /* CLEANER (RING_SUB, measured, black box; PROVENANCE 13): with the ring modulator on (any value from 1) the
         * reference's sub joins osc 1. (a) The sub is under osc 1's side of the balance and under the osc volume once
         * more (saws, sub 81, ring 1-31 against ring 0: balance 32 / 64 / 96 / 112 / 127 -0.8 / -1.8 / -8.0 / -13.0 /
         * -23 dB, the balance's osc 1 curve -1.0 / -2.0 / -8.1 / -14.1 / mute; osc volume 48 / 32 / 16 -4.4 / -9.0 /
         * -15.3 dB, its law -4.7 / -9.5 / -16.9; with the ring at 0 the sub keeps its level). (b) The ring's first
         * factor is osc 1 + sub: with sines at 220 / 330 Hz the lines of sub x osc 2 (440, 660, 880 ... Hz) stand at
         * the sub's level against osc 1 as in the mix (within 0.2-0.6 dB: sub volume 40-127, square and triangle,
         * balance 0 and 127, osc 1 a sine, saw or square), the sub's part under the osc volume, osc 1's not. Before:
         * neither (a test patch, balance 64, ring 31: the sub's lines +2 dB, 99-157 Hz +2 dB in its chords). Both set by
         * ring_sub_set (in the flash); the voice multiplies the sub's block by (sub_gb - ring_s x osc 2) before the
         * mix (sy_voice.c sub_ring). ASSUMPTIONS: without osc 1's high-key read level (hi_gain); not in the split
         * routing (each oscillator on its own side there; not measured) */
        b->ring_s = 0.f;
        if (RING_SUB && b->ring_g != 0.f && b->sub_g != 0.f) SY_FAR(ring_sub_set)(s, b, ov, hg1, E(OSC_VOL));
        b->noise_g = hyb_tab(HYB_RING_G, sy_clampf(E(NOISE_VOL), 0.f, 127.f)) / HYB_RING_G[127] * 0.1677f * 0.927f;     /* rms -24.7 dB at full (A/B); -0.66 dB: re-read, osc volume 0-64 */
        {   /* the noise colour (blk_cache; the fields the mode leaves alone stay as they were); with the matrix's
             * offset (91: MEASURED per voice) the voice's own */
            const struct sy_bc *nc = &bc;
            struct sy_bc t;
            if (m[NOISE_COL] != 0.f) { SY_FAR(ncol)(s, E(NOISE_COL), &t); nc = &t; }
            b->n_mode = nc->n_mode;
            b->n_gain = nc->n_gain;
            b->nf_gain = nc->nf_gain;
            if (nc->n_mode == 1) { b->n_lpa = nc->n_lpa; b->n_lpb = nc->n_lpb; }
            else if (nc->n_mode == 2) b->n_hpa = nc->n_hpa;
        }
        b->olp_a = bc.olp_a;
        b->olp_b = bc.olp_b;
        b->use_input = s->patch[IN_MODE] == 1;
        b->in_g = ov * 0.607f;          /* rate.json dc_level, open.json punch: -4.3 dB re the osc volume's law */
        {   /* the start transient (sy_voice.c): its size by the punch (blk_cache) and the osc volume (not the
             * balance), its time by the pitch */
            b->clk_g = bc.pk * ov * CLK_K;
            /* (its sign: the oscillators' own at the note's first frame, sy_voice.c; turned over while osc 1 is
             * heard with a shape from 105 under an init phase: read per shape; there the pulse wave starts on its
             * edge) */
            if (v->iph && shape1 >= 105.f && b->g1 != 0.f) b->clk_g = -b->clk_g;
            b->clk_rate = vc[vi].rate;  /* (per osc 1 pitch, above) */
        }
    }

    /* the filters: cutoff in steps (base + envelope + keyfollow + LFO 2 + matrix), summed and limited at the ends */
    velf = 1.f;
    {
        float vd = m[B_V_FE1] != 0.f ? SY_FAR(vd1_of)(E(B_V_FE1)) : bc.vd1, sv = ((float)vel - 1.f) / 126.f;   /* (the amount's
                                         * share, blk_cache) */
        velf = vd >= 0.f ? (1.f - vd) + vd * sv : 1.f + vd * sv;   /* (below 64: velocity 1 full, 127 less: filt2.json vel_fenv) */
    }
    c1 = E(CUT1) + (s->patch[B_FPOL1] ? 1.f : -1.f) * sy_tab(s, T_FENV_G, sy_clampf(E(FENV1), 0.f, 127.f)) * fe * velf
       + (E(FKF1) - 64.f) / 64.f * (key - (float)s->patch[B_FBASE]) * (12.9f / 12.f);
    d = E(L2_CUT1) - 64.f;
    c1 += d * sy_absf(d) / 63.f * lfo2;
    {
        float vd = (E(B_V_FE2) - 64.f) / 63.f, sv = ((float)vel - 1.f) / 126.f;
        float vf2 = vd >= 0.f ? (1.f - vd) + vd * sv : 1.f + vd * sv;
        c2 = (s->patch[B_LINK] ? s->pv[CUT1] + link_off(E(CUT2) - 64.f) + m[CUT1] : E(CUT2))
           + (s->patch[B_FPOL2] ? 1.f : -1.f) * sy_tab(s, T_FENV_G, sy_clampf(E(FENV2), 0.f, 127.f)) * fe * vf2
           + (E(FKF2) - 64.f) / 64.f * (key - (float)s->patch[B_FBASE]) * (12.9f / 12.f);
        d = E(L2_CUT2) - 64.f;
        c2 += d * sy_absf(d) / 63.f * lfo2;
    }
    b->m1 = s->patch[F1_MODE] & 7;
    b->m4_g = 0.f;
#ifndef F1M6_DIST
#define F1M6_DIST 1                     /* CLEANER: filter 1's mode 6 with the distortion on is silent (PROVENANCE 14) */
#endif
#if F1M6_DIST
    /* CLEANER: mode 6 in a voice with the distortion on (B 100 type 1-11) puts out nothing (measured, black box:
     * tools/measure/f1stale.py, PROVENANCE 14: -164 dB, the renderer's floor, for every type 1-11, intensity, routing
     * serial 4 / 6 and parallel; split passes as before; type 0 and 12+ the bypass): filter 1 as mode 4 in a voice.
     * Filter 2's share of the oscillators (balance above 64, parallel) stays. The static path sets it back */
    if (b->m1 == 6 && s->patch[ROUTE] != 3 && (unsigned)s->patch[PB + 100] - 1u < 11u) b->m1 = 4;
#endif
    b->m2 = s->patch[F2_MODE] > 3 ? 3 : s->patch[F2_MODE];
    {
        float r1 = E(RESO1) + (E(L1_RESO) - 64.f) * 0.5f * lfo1 + 2.f * vel_bip(E(B_V_RES1), vel);
        float r2 = E(RESO2) + 2.f * vel_bip(E(B_V_RES2), vel);   /* (velocity: 2 steps, open.json) */
        int ser6 = s->patch[ROUTE] == 1, ana = b->m1 == 5 || b->m1 == 7;
        float cdist = 2.f * sy_absf(c2 - c1);   /* (the cutoffs' distance in half steps: one value of the linked offset) */
        int ser2 = s->patch[ROUTE] <= 1 && !ana && cdist < 5.f;   /* (filter 2's k and h set again below, W) */
        b->g1e = filt_g(s, c1, 1);
        if (ser6) b->pg1 = sy_tab(s, T_RESO_PG, sy_clampf(r1, 0.f, 127.f));   /* (k and h: K6 below; pg as filt_k's) */
        else filt_k(s, c1, r1, b->g1e, &b->h1e, &b->k1, &b->pg1, 1);
        {   /* the band pass's level per resonance (dB re the model, read at every value, cutoff 40-100 alike:
             * the reference's law is piecewise, with breaks every 16 steps and at 104 and 112) */
            static const float BPC[128] = {
            -0.00f, 0.01f, 0.01f, 0.02f, 0.02f, 0.02f, 0.03f, 0.03f,
            0.03f, 0.04f, 0.04f, 0.04f, 0.04f, 0.04f, 0.04f, 0.04f,
            0.04f, 0.14f, 0.23f, 0.30f, 0.35f, 0.39f, 0.42f, 0.43f,
            0.44f, 0.43f, 0.41f, 0.38f, 0.35f, 0.30f, 0.25f, 0.19f,
            0.12f, 0.16f, 0.18f, 0.20f, 0.21f, 0.22f, 0.22f, 0.21f,
            0.20f, 0.18f, 0.15f, 0.12f, 0.09f, 0.05f, 0.01f, -0.04f,
            -0.09f, -0.12f, -0.16f, -0.20f, -0.25f, -0.30f, -0.35f, -0.41f,
            -0.46f, -0.52f, -0.59f, -0.65f, -0.72f, -0.79f, -0.87f, -0.94f,
            -1.02f, -0.71f, -0.43f, -0.18f, 0.05f, 0.26f, 0.45f, 0.62f,
            0.78f, 0.92f, 1.04f, 1.15f, 1.25f, 1.34f, 1.42f, 1.49f,
            1.54f, 1.64f, 1.72f, 1.79f, 1.86f, 1.92f, 1.98f, 2.03f,
            2.07f, 2.11f, 2.14f, 2.17f, 2.19f, 2.20f, 2.21f, 2.21f,
            2.21f, 2.42f, 2.57f, 2.68f, 2.75f, 2.77f, 2.77f, 2.72f,
            2.63f, 2.21f, 1.75f, 1.25f, 0.68f, -0.51f, -1.77f, -3.19f,
            -4.94f, -4.44f, -4.00f, -3.60f, -3.25f, -3.12f, -2.73f, -2.52f,
            -2.35f, -2.25f, -2.19f, -2.16f, -2.18f, -2.30f, -2.47f, -2.69f};
            b->bp1 = BP0 * memo_db2lin(vc[vi].mm[MM_BP1], BPC[(int)sy_clampf(r1 + 0.5f, 0.f, 127.f)]);
            b->bp2 = BP0 * memo_db2lin(vc[vi].mm[MM_BP2], BPC[(int)sy_clampf(r2 + 0.5f, 0.f, 127.f)]);
        }
        b->g2e = FILT2_TAB == 1 && c2 == c1 ? b->g1e : filt_g(s, c2, FILT2_TAB);   /* (the linked filters often meet) */
        if (ana) ;                      /* (the analog modes set filter 2's k, h and pg themselves, below) */
        else if (ser2) b->pg2 = sy_tab(s, T_RESO_PG, sy_clampf(r2, 0.f, 127.f));   /* (k and h: W below) */
        else filt_k(s, c2, r2, b->g2e, &b->h2e, &b->k2, &b->pg2, FILT2_TAB);
        if (ser6) {     /* serial 6, filter 1's two stages share one damping of their own (a fit
                                         * of two equal stages on a voice's noise, cutoff 60); the passband as one stage */
            static const float K6[20][2] = {{0, 1.4087f}, {16, 1.2269f}, {24, 1.137f}, {32, 0.9949f}, {40, 0.8413f},
                {48, 0.6896f}, {56, 0.5774f}, {64, 0.491f}, {72, 0.4052f}, {80, 0.3345f}, {88, 0.2859f}, {96, 0.2376f},
                {100, 0.2134f}, {104, 0.1935f}, {108, 0.1798f}, {112, 0.1661f}, {116, 0.1525f}, {120, 0.1388f},
                {124, 0.1252f}, {127, 0.115f}};
            float rr = sy_clampf(r1, 0.f, 127.f), kk;
            if (vc[vi].k6r != rr) {     /* (per resonance) */
                int i = 0;
                while (i < 18 && rr > K6[i + 1][0]) i++;
                vc[vi].k6r = rr;
                vc[vi].k6k = K6[i][1] + (rr - K6[i][0]) / (K6[i + 1][0] - K6[i][0]) * (K6[i + 1][1] - K6[i][1]);
            }
            kk = vc[vi].k6k;
            b->k1 = kk;
            b->h1e = 1.f / (1.f + kk * b->g1e + b->g1e * b->g1e);
        }
        b->f1x = 0.f;
#if F1_STALE
        if (b->m1 == 4 || b->m1 == 6) SY_FAR(f1_stale)(s, b, E(OSC_VOL));   /* (PROVENANCE 14b) */
#endif
        if (ser2) {                     /* serial, filter 2's damping as at reso2 x
                                         * (1 - 0.37 reso1 / 127 x w), w by the cutoffs' distance cdist (1 when equal, 0 from
                                         * 2.5 steps apart); its passband as at reso2 (poles read from the free decay) */
            static const float W[6] = {1.f, 0.78f, 0.59f, 0.31f, 0.04f, 0.f};
            float w, pg;
            if (vc[vi].wd != cdist) {   /* (per distance) */
                int i = (int)cdist;
                vc[vi].wd = cdist;
                vc[vi].ww = W[i] + (cdist - (float)i) * (W[i + 1] - W[i]);
            }
            w = vc[vi].ww;
            filt_k(s, c2, sy_clampf(r2, 0.f, 127.f) * (1.f - 0.37f / 127.f * sy_clampf(r1, 0.f, 127.f) * w),
                           b->g2e, &b->h2e, &b->k2, &pg, FILT2_TAB);
        }
        if (ana) VB_CALL(lad_set)(s, b, m, vi, c1, r1, r2);   /* (wave 3: in the flash, VB_FLASH) */
    }
    routing = s->patch[ROUTE] > 3 ? 0 : s->patch[ROUTE];
    b->route = routing;
    b->tk = 0;
    b->co_q = sy_clampf(s->osc_clean, 0.f, 1.f);
    b->co_v6 = 0;
    b->pl_v6 = CO_V6 && s->v6 && b->co_q == 0.f;   /* (cleared below where osc 1 and 2 run on the own clock) */
    if (HYB_OSC_TICK && !b->use_input && !b->sync && b->fm_i == 0.f && routing != 3) {
        /* CLEANER: osc 1 and 2 the own oscillator (sy_voice.c, sy_cln_osc.inc). Its clock, measured (black box,
         * tools/measure/osc/clock.py, sy_cln_osc.h): P ticks a cycle of osc 1, P = CO_SEG_N[k] / 2^o for osc 1's pitch
         * in octave o above CO_F0 x fs, k its segment there (edges CO_SEG_E); the clock at f1 P, under 2 fs */
        float q = sy_maxf(hz1, 0.01f) / (CO_F0 * s->fs), n;
        int io = 0, im = 0;
        while (q >= 2.f) { q *= 0.5f; io++; }
        while (q < 1.f) { q *= 2.f; io--; }
        while (im < CO_SEG_K - 1 && q >= CO_SEG_E[im]) im++;
        {   /* (CEXP2(-io) without the polynomial: at a whole number it is 2^-io exactly; |io| < 126 here) */
            sy_fu u;
            u.i = (127 - io) << 23;
            n = CO_SEG_N[im] * u.f;
        }
        if (CO_V6 && s->v6 && b->co_q == 0.f) {
            /* CLEANER: the 6-voice state (measured, black box: tools/measure/osc/v6.py; PROVENANCE 9d). With 6 or more
             * voices on (releases included, as VC6_GAIN), every voice's oscillators read as on a clock of half the
             * ticks a cycle (the images at (P/2 -+ 1) f1), each step a straight line over one tick (no 5-tick
             * response) and no emphasis: osc 1's lines within 0.05 dB below 19 kHz at keys 36-120, the images
             * too. Osc 2 reads darker than that: its own taps (CO_V6_C1, C2). ORIGINAL only: CLEAN keeps its step */
            b->co_v6 = 1;
            n *= 0.5f;
        }
        if (n < 2.f) n = 2.f;
        if (n > 65536.f) n = 65536.f;
        {
            uint32_t ni = (uint32_t)(n + 0.5f);
            float rate = hz1 * (float)ni;
            b->tk_inc1 = (uint32_t)((1u << CO_PH_BITS) - 1u) / ni << (32 - CO_PH_BITS);   /* (osc 1 a tick: a whole
                                         * number of the phase's last bit, under a P-th of a cycle) */
            b->tk_r = rate / (2.f * s->fs);
            b->tk_inc2 = (uint32_t)sy_minf(4294967040.f, hz2 / rate * 4294967296.f);
            if (b->tk_r < 1.f) {
                b->tk = 1;
                b->pl_v6 = 0;
                {   /* the pulse's width in whole ticks, its fraction of a tick kept for the edge's tick (measured:
                     * pulse.py); osc 2 by its own ticks a cycle */
                    float n2 = rate / sy_maxf(hz2, 0.01f), q1 = sy_clampf(pw1, -127.f, 127.f) * (float)ni / 254.f;
                    float q2 = sy_clampf(pw2, -127.f, 127.f) * n2 / 254.f;
                    b->o1.tkn = (float)ni; b->o1.tkm = (float)sy_floori(q1); b->o1.tkf = q1 - b->o1.tkm;
                    b->o1.d = (uint32_t)((0.5f + b->o1.tkm / (float)ni) * 4294967040.f);
                    if (n2 >= 2.f) {
                        b->o2.tkn = n2; b->o2.tkm = (float)sy_floori(q2); b->o2.tkf = q2 - b->o2.tkm;
                        b->o2.d = (uint32_t)((0.5f + b->o2.tkm / n2) * 4294967040.f);
                    }
                }
                {   /* high pitches (measured: level.py): each oscillator's level falls CO_HI_K a semitone from pitch
                     * CO_HI_P0 on (m), instead of the plain oscillators' (hi_gain); the emphasis by the higher m of the
                     * two: (eb - dm) x[n] - (ea + dm) x[n-1], eb not under 1 (the highs keep their gain, the lows fall) */
                    float m1 = sy_clampf(1.f - CO_HI_K * (hp1 - CO_HI_P0), 0.f, 1.f), m2 = sy_clampf(1.f - CO_HI_K * (hp2 - CO_HI_P0), 0.f, 1.f);
                    float dm = 1.f - sy_maxf(m1, m2);
                    b->g1 *= m1 / sy_maxf(hg1, 1e-6f);
                    b->g2 *= m2 / sy_maxf(hg2, 1e-6f);
                    b->tk_ea = b->co_v6 ? 0.f : CO_EA + dm;   /* (the 6-voice state: none, at every key) */
                    b->tk_eb = b->co_v6 ? 1.f : sy_maxf(1.f, CO_EB - dm);
                    b->dip = 0;
                }
            }
        }
    }
#if PW_PH
    {   /* (pw_ph: a change of the pulse width moves the oscillators' phases) */
        const float w1 = sy_clampf(pw1, -127.f, 127.f) * (1.f / 254.f), w2 = sy_clampf(pw2, -127.f, 127.f) * (1.f / 254.f);
        const float d1 = w1 - v->pwq1, d2 = w2 - v->pwq2;
        v->pwq1 = w1; v->pwq2 = w2;
        if (v->started && (d1 != 0.f || d2 != 0.f)) SY_FAR(pw_ph)(b, d1, d2, p1 - key, p2 - key);
    }
#endif
    {
        float fb = sy_clampf(E(FBAL), 0.f, 127.f);
#if BAL_LAW
        /* the balance (measured, black box: cleaner/tools/measure/bal_law.py; PROVENANCE 10): one curve P, the share
         * of the side the balance moves away from, P(n) / 256 = 2n (n 0-16), 32 + 4 (n - 16) (16-32), 96 + 5 (n - 32)
         * (32-64) over the distance n from the middle (64); A = P(64 - fb) below the middle, C = P(fb - 64) above
         * (127: 1). Serial: filter 1 x A + filter 2 x (1 - A) below, above filter 2 alone from C x the oscillators
         * + (1 - C) x filter 1 (the sum constant: read on the high bands of a high-passed filter 1). Parallel:
         * 0.5 + 0.2071 A and 0.5 (1 - A) below, 0.5 (1 - C) and 0.5 + 0.2071 C above (0.7071 at the ends, 0.5
         * in the middle); split the same x 2.523 (each side alone; filter 2 read on the right with osc 2) */
        {
            const float n = fb <= 64.f ? 64.f - fb : fb - 64.f;
            float p = n <= 16.f ? 2.f * n : n <= 32.f ? 32.f + 4.f * (n - 16.f) : 96.f + 5.f * (n - 32.f);
            if (fb > 126.f) p += 5.f * (fb - 126.f);   /* (127: the full share, 10 / 256 the last step) */
            p *= 1.f / 256.f;
            if (routing <= 1) {                     /* serial: filter 1 -> (saturation) -> filter 2 */
                if (fb <= 64.f) { b->bal_a = p; b->bal_b = 1.f - p; b->bal_c = 0.f; }
                else { b->bal_a = 0.f; b->bal_b = 1.f; b->bal_c = p; }
            } else {                                /* parallel / split: both from the mix */
                const float gs = routing == 3 ? 2.523f : 1.f;
                b->bal_c = 1.f;
                if (fb <= 64.f) { b->bal_a = gs * (0.5f + 0.20711f * p); b->bal_b = gs * 0.5f * (1.f - p); }
                else { b->bal_a = gs * 0.5f * (1.f - p); b->bal_b = gs * (0.5f + 0.20711f * p); }
            }
        }
#else
        if (routing <= 1) {                         /* serial: filter 1 -> (saturation) -> filter 2 */
            if (fb <= 64.f) { b->bal_a = 1.f - fb / 64.f; b->bal_b = fb / 64.f; b->bal_c = 0.f; }
            else { b->bal_a = 0.f; b->bal_b = 1.f; b->bal_c = (fb - 64.f) / 63.f; }
        } else {                                    /* parallel / split: both from the mix */
            float t = fb / 127.f;
            b->bal_c = 1.f;
            b->bal_a = t <= 0.5f ? 0.7071f - 0.4142f * t : (1.f - t) * 1.f;
            b->bal_b = t >= 0.5f ? 0.7071f - 0.4142f * (1.f - t) : t * 1.f;
            if (routing == 3) {                     /* split: filt.json routing 3, filter 1 on the left +4.5 dB
                                                     * at 0, +1.5 at 64 (ASSUMPTION: filter 2 on the right alike) */
                b->bal_a = 1.68f * sy_cos2pi(0.25f * t);
                b->bal_b = 1.68f * sy_sin2pi(0.25f * t);
            }
        }
#endif
    }

    /* the saturation stage: type and the drive (osc volume above 64) */
    {
        int t = s->patch[SAT_TYPE];
        float drv = sy_clampf((E(OSC_VOL) - 64.f) / 63.f, 0.f, 1.f), dv = 64.f + 63.f * drv;
        b->sat = t > 14 ? 0 : t;
        b->sat_w = drv;
#ifdef HYB_MULTI
        b->sat_lut = s->sat_lut;
#endif
        if (t == 7) {   /* type 7's gain and its lower half's slope (sy_voice.c), fitted per drive 64, 68 .. 124, 127 */
            static const float G7[17] = {1.0023f, 1.0648f, 1.1273f, 1.1896f, 1.2525f, 1.3152f, 1.3779f, 1.4407f, 1.5035f,
                1.5662f, 1.6290f, 1.6918f, 1.7546f, 1.8172f, 1.8799f, 1.9426f, 2.0053f};
            static const float M7[17] = {0.9954f, 0.8709f, 0.7464f, 0.6217f, 0.4971f, 0.3724f, 0.2476f, 0.1229f, -0.0019f,
                -0.1267f, -0.2514f, -0.3762f, -0.5010f, -0.6258f, -0.7505f, -0.8753f, -1.0001f};
            float q = (dv - 64.f) * 0.25f, f;
            int k = (int)q;
            if (k >= 15) { k = 15; f = (dv - 124.f) / 3.f; } else f = q - (float)k;
            b->sat_a7 = G7[k] + f * (G7[k + 1] - G7[k]);
            b->sat_b7 = M7[k] + f * (M7[k + 1] - M7[k]);
        }
        if (t >= 1 && t <= 4 && !CTL_PAR && (s->sat_type_built != t || s->sat_drive_built != drv)) {   /* (CTL_PAR:
                                         * built after, by sy_render) */
            SY_FAR(sy_sat_build)(s, t, drv);
            s->sat_type_built = (uint8_t)t;
            s->sat_drive_built = drv;
        }
        /* sat.json static_curve: the digital type's height, the shaper's period and height, the bit steps */
        /* (the shaper's period: type 6's own, below; read by type 6 only) */
        b->sat_b = t == 5 ? (dv < 96.f ? 0.122f - 0.043f * (dv - 64.f) / 32.f : 0.079f - 0.001f * (dv - 96.f) / 31.f)
                          : 0.081f - 0.020f * drv;
        b->sat_b *= SAT_TRIM;
        b->sat_a = t == 7 ? bc.op7 : 0.f;
        if (t == 5 || t == 6 || (t >= 8 && t <= 14)) VB_CALL(sat_set)(s, b, vi, t, drv, dv, key);   /* (wave 3: in the
                                         * flash, VB_FLASH) */
    }

    /* amp: envelope x velocity x volume x LFO 1 filter gain, then the pan law */
    {
        /* the amount scales the velocity, one curve gives the level (read per amount and velocity): above 64
         * the effective velocity is 127 - (127 - vel) (a - 64) / 63, below 64 127 - (vel - 1) (64 - a) / 64 (at least
         * 5.4: amount 0 at velocity 127 reads -53 dB) */
        float va = E(B_V_AMP), ev = 127.f;
        if (va > 64.f) ev = 127.f - (127.f - (float)vel) * (va - 64.f) * (1.f / 63.f);
        else if (va < 64.f) ev = sy_maxf(5.4f, 127.f - ((float)vel - 1.f) * (64.f - va) * (1.f / 64.f));
        if (va != 64.f) gain_db += sy_tab(s, T_VELAMP_DB, ev);
        d = E(L1_FGAIN) - 64.f;                     /* a tremolo between two plateaus (open.json lfo_dest): the low one
                                                     * -6 dB per 16 steps (silent at 63), the high one -2.1 .. -3.9 dB */
        if (d != 0.f) {
            float ad = sy_absf(d), u = d > 0.f ? lfo1 : -lfo1;
            /* from 16 the low point falls linearly (0.75 - ad/64: 0 at 48, below it the gain clips at 0
             * for part of the cycle) */
            float lo = ad <= 16.f ? memo_exp2(vc[vi].mm[MM_FLO], -ad / 16.f) : 0.75f - ad / 64.f;
            float hi = memo_db2lin(vc[vi].mm[MM_FHI], ad <= 16.f ? -2.1f * ad / 16.f : ad <= 32.f ? -2.1f - 0.65f * (ad - 16.f) / 16.f
                                                                                    : -2.75f - 1.18f * (ad - 32.f) / 31.f);
            fgain = sy_maxf(0.f, hi + (lo - hi) * (1.f - u) * 0.5f);
        }
        {   /* the patch and part volume act after the part's distortion (s->part_g in sy_fx_render); a voice
             * keeps only its own modulation of the patch volume */
            float base = bc.pg_base;    /* patch_gain(pv[PATCH_VOL]) (blk_cache; unmodulated, x / x = 1 exactly) */
            vol = (base > 0.f ? (m[PATCH_VOL] == 0.f ? 1.f : SY_FAR(patch_gain)(E(PATCH_VOL)) / base) : 1.f) * (1.f / 0.86f);
        }                    /* (the part's route has -0.86: the measured levels include it) */
        if (uni_n > 1) {                            /* the unison voices' level (mod.json unison) */
            /* tapered by the slot: -2.5 dB x |x| (x = -1 .. 1 across the slots), the outer slots at UNI_DB */
            float x = sy_absf(uni_x(uni_n, v->uni_i));
            gain_db += UNI_DB + UNI_TAPER * (1.f - x);
            /* (from 6 voices on every voice reads 0.5 dB louder: that is the part's gain with 6 or more voices on,
             * VC6_GAIN in synth_block) */
        }
        gain_db = memo_db2lin(vc[vi].mm[MM_GAIN], gain_db);   /* (from here the gain, linear) */
#ifdef SY_UNI_CAP
        uramp = v->ufb;
        if (v->ufb) {                   /* its note's unison level correction ramps (synth.c uni_level), a voice taken
                                         * fades out (uni_fade): straight in power block by block (the note's power
                                         * stays: the fading voice's and the others' changes add to 0), linear inside
                                         * a block (an attack too: no curve in its block then); ended at the fade's end */
            float p = v->ug * v->ug, x = sy_maxf(v->ug, 1e-3f);
            int k;
            p += (v->ugt * v->ugt - p) / (float)v->ufb;
            if (v->ug <= 0.f) x = 0.5f * (1.f + p);   /* (a voice faded in from silence, uni_live: Newton from
                                         * above, as 1e-3 would land far off in three steps) */
            for (k = 0; k < 3; k++) x = 0.5f * (x + p / x);   /* (its root: Newton from the last block's) */
            v->ug = p > 0.f ? x : 0.f;
#if UNI_FADE_S
            if (v->ufade) v->ug = SY_FAR(uni_fade_s)(v);   /* (a voice taken: the smooth curve instead) */
#endif
            if (!--v->ufb) {
                v->ug = v->ugt;
                if (v->ufade) { v->on = v->gate = 0; v->aenv.stage = v->fenv.stage = 0; }
            }
        }
        gain_db *= v->ug;               /* (1 with the caps off: x 1, the output to the bit) */
#endif
        amp = ae * vol * gain_db * fgain;
        pan = E(PAN) + vel_lin(E(B_V_PAN), vel);
        d = E(L2_PAN) - 64.f;
        plfo = (d < 0.f ? -1.f : 1.f) * sy_tab(s, T_PAN_LFO, sy_absf(d)) * lfo2;
        uspr = 0.f;
        if (uni_n > 1)                              /* spread 64 already puts two voices hard left and right */
            /* on the detune's slots: an even count leaves the centre free (4: -1 -1/2 1/2 1; read per line
             * at spread 16-127, 4/6/8 voices: within 0.5 dB) */
            uspr = s->pv[UNI_SPREAD] * (128.f / 127.f) * uni_x(uni_n, v->uni_i);
        pan = sy_clampf(uni_pan(pan + plfo, uspr), 0.f, 127.f);
        float pl = pan_l(pan), pr = pan_r(pan);
        b->gle = amp * pl;
        b->gre = amp * pr;
        b->knee = (int)(v->aenv.knee * SY_BLOCK + 0.5f);
        b->expo = v->aenv.stage != 1;
        b->atk = atk0 && v->aenv.alen > 0.f;
#ifdef SY_UNI_CAP
        if (uramp) b->atk = 0;
#endif
        if (b->knee < SY_BLOCK || b->atk) {     /* the attack ends inside the block: full level at the knee */
            float full = vol * gain_db * fgain;
            b->gkl = b->afl = full * pl;
            b->gkr = b->afr = full * pr;

            b->an0 = v->aenv.an - (float)SY_BLOCK;
            b->al0 = v->aenv.al0; b->alen = v->aenv.alen;
        }
    }
    if (!v->started && v->iph == 0 && !v->used) SY_FAR(voice_start_phase)(s, v);   /* (S1: a note's first block, in the flash) */
    /* 0.93: the pan glides across the block with the amp, all its parts (vr_amp: gl, gr -> gle, gre per sample; the
     * attack's level afl, afr from the last block's). MEASURED (the reference, a sine voice, LFO 1 a saw through the
     * matrix on the pan): the pan moves smoothly, and at the saw's reset it crosses in 8
     * frames (linear steps, at a position inside the block: 0, 32, 64, 88 seen); until 0.92 the pan's own and matrix
     * part stepped at the block's start: a click on every block while it moved (a saw LFO, the mod wheel).
     * a jump spreads over the block (128 frames), not over 8 */
    if (!b->atk) {                      /* (outside the attack: the level kept for an attack that follows) */
        float full = vol * gain_db * fgain;
        v->afl = full * pan_l(pan);
        v->afr = full * pan_r(pan);
    } else if (!v->started || s->pv_jump) {   /* (an attack's first block, a controller's step: from its own level) */
        v->afl = b->afl;
        v->afr = b->afr;
    }
    if (!v->started || s->pv_jump) {                /* the first block (or a controller's step): no interpolation */
        v->g1 = b->g1e; v->h1 = b->h1e; v->g2 = b->g2e; v->h2 = b->h2e;
        v->gl = b->gle * (v->aenv.prev / sy_maxf(ae, 1e-9f));
        v->gr = b->gre * (v->aenv.prev / sy_maxf(ae, 1e-9f));
        if (ae <= 0.f) v->gl = v->gr = 0.f;
        v->started = 1;
        v->used = 1;
    }
    (void)p2;
    for (i = 0; i < nt; i++) m[touched[i]] = 0.f;   /* (m all 0 again for the next voice) */
}

/* ---- the effects' matrix offsets after the voice started last has ended. MEASURED: the
 * reference goes on computing that voice's matrix for the part's effects, with the voice silent: its LFOs run on (an
 * LFO on the chorus depth or the delay feedback keeps moving the effects' tail for seconds, with the release 0 or 80
 * alike), key and velocity stay its own, the controllers act as they move, a patch load takes the new patch's slots
 * (no slot: 0). Here its LFOs' states, key and velocity are kept from its last block (voice_block) and run on
 * alone, without touching the voice. ASSUMPTION: the envelopes as sources 0 (they ended); the LFOs free running at
 * the patch's rate and symmetry (no matrix on them here), the clocked LFO's offset as voice_block's, the arpeggiator's
 * phase not taken; before the first note after power-on no offsets (the reference has some voice's even then) */
static SY_LATE void fx_tail(sy_synth *s)   /* (in the flash: once a block, after the notes) */
{
    static const uint8_t RATE[3] = {L1_RATE, L2_RATE, B_L3_RATE}, SHAPE[3] = {L1_SHAPE, L2_SHAPE, B_L3_SHAPE};
    static const uint8_t MODE[3] = {L1_MODE, L2_MODE, B_L3_MODE}, KF[3] = {L1_KF, L2_KF, B_L3_KF};
    static const uint8_t CLK[3] = {B_CLK_L1, B_CLK_L2, B_CLK_L3}, ENV[3] = {L1_ENV, L2_ENV, 0}, SYM[3] = {L1_SYM, L2_SYM, 0};
    static const uint8_t TRG[3] = {L1_TRIG, L2_TRIG, 0};
    float o[SY_FXN + 2], amt[11];       /* (+ the input ring modulator's and the dry level's: rm_off, dry_off) */
    uint8_t src[11], by[11];
    int i, k, lfo = 0;
#ifdef HYB_SHIMMER
    float so[3] = {0.f, 0.f, 0.f};
#define SHX_RT(i) (s->shx_rt[i] >= 1 && s->shx_rt[i] <= 2)
#endif
    NO_UNROLL for (k = 0; k < SY_FXN + 2; k++) o[k] = 0.f;
    NO_UNROLL for (i = 0; i < 11; i++) {          /* the slots on the effects (as matrix) and the LFOs they read */
        int d, b;
        if (i < 9) { src[i] = s->patch[PB + SLOT[i][0]]; d = s->patch[PB + SLOT[i][1]]; amt[i] = s->pv[PB + SLOT[i][2]]; }
        else { src[i] = (uint8_t)(i + 12); d = s->patch[i == 9 ? B_L1_DEST : B_L2_DEST]; amt[i] = s->pv[i == 9 ? B_L1_AMT : B_L2_AMT]; }
        b = d > 0 && d < 120 ? DEST[d] : 0;
        by[i] = (uint8_t)(!src[i] ? 0 : b == PB + 99 ? SY_FXN + 1 : b == B_OUT2 ? SY_FXN + 2
                          : SY_FAR(sy_fx_slot)(b));   /* (its place in fxm + 1; 0: not the effects') */
        if (by[i] && src[i] >= 21 && src[i] <= 23) lfo |= 1 << (src[i] - 21);
#ifdef HYB_SHIMMER
        if (SHX_RT(i) && src[i] >= 21 && src[i] <= 23) lfo |= 1 << (src[i] - 21);   /* (the shimmer's targets too) */
#endif
    }
#ifdef HYB_SHIMMER
    if (SHX_RT(11)) lfo |= 4;           /* (LFO 3 on the shimmer) */
#endif
    NO_UNROLL for (k = 0; k < 3; k++) {           /* those LFOs one block on (voice_block's second reading) */
        sy_lfo_t *l = &s->fx_lfo[k];
        const int envm = ENV[k] && s->patch[ENV[k]], ck = s->patch[CLK[k]], mono = s->patch[MODE[k]] != 0;
        const int clkd = ck >= 1 && ck <= 19 && !(TRG[k] && s->patch[TRG[k]]) && !envm;
        float ph, lh, x;
        if (!(lfo >> k & 1)) continue;
        lh = !mono || clkd ? SY_FAR(lfo_hz)(s, s->pv[RATE[k]], ck, s->patch[KF[k]], (float)s->fx_key) : 0.f;
        if (mono) { const sy_lfo_t *g = &s->glfo[k]; l->ph = g->ph; l->sh_from = g->sh_from; l->sh_to = g->sh_to; l->done = g->done; }
        else lfo_advance(l, envm ? 2.f * lh : lh, s->bdt, envm);
        ph = l->ph;
        if (clkd) { ph = ph + 0.445f - LFO_CLK_LAG * s->bdt * lh; ph -= (float)sy_floori(ph); }
        x = SY_FAR(lfo_shape)(l, ph, s->patch[SHAPE[k]],
                              SYM[k] ? sy_clampf(0.5f + (s->pv[SYM[k]] - 64.f) / 126.f, 0.f, 1.f) : 0.5f);
        l->out = envm ? (l->done ? 0.f : x + 1.f) : x;
    }
    NO_UNROLL for (i = 0; i < 11; i++) {
        const int c = src[i];
        float x;
#ifdef HYB_SHIMMER
        if (!by[i] && !(SHX_RT(i) && c)) continue;
#else
        if (!by[i]) continue;
#endif
        if (c == 19 || c == 20) x = 0.f;
        else if (c >= 21 && c <= 23) x = 0.5f * s->fx_lfo[c - 21].out;
        else if (c == 24 || c == 25) x = (float)s->fx_vel / 127.f;
        else if (c == 26) x = ((float)s->fx_key - 60.f) / 64.f;
        else x = SY_FAR(src_value)(s, 0, c);   /* (the controllers, bend, pressure, the constant: no voice read) */
#ifdef HYB_SHIMMER
        if (SHX_RT(i)) so[s->shx_rt[i] - 1] += x * (amt[i] - 64.f) * 2.f;
        if (!by[i]) continue;
#endif
        o[by[i] - 1] += x * MX_AMT(amt[i]) * 2.f;
    }
#ifdef HYB_SHIMMER
    if (SHX_RT(11)) so[s->shx_rt[11] == 2 ? 2 : 0] += 0.5f * s->fx_lfo[2].out * s->pv[B_L3_AMT];   /* (as fx_take's) */
    s->shm[0] = so[0]; s->shm[1] = so[1]; s->shm[2] = so[2] * (2.f / 127.f);
#endif
    k = 0;
    NO_UNROLL for (i = 0; i < SY_FXN; i++) { s->fxm[i] = o[i]; k |= o[i] != 0.f; }
    s->fx_mod = (uint8_t)k;
    s->rm_off = B2C(off256)(o[SY_FXN]);
    s->dry_off = B2C(off256)(o[SY_FXN + 1]);
}

/* ---- the part's input ring modulator (B 99) on its voices, and its dry level (B 58), with the matrix's offsets
 * (96, 113). MEASURED: B 99 takes the voices' sum down before the distortion, the chorus
 * and the delay / reverb: 1 - 0.293 v / 32 up to 32, 0.707 (64 - v) / 32 up to 64, nothing from 64 to 127 (a sine
 * voice, to 5 decimals; with no input nothing of the product is left). The
 * matrix moves both as the byte (the twin), the offsets the voice started last's (as the effects'). (flash):
 * the reference's change runs linearly over the block and between the dry level's steps (read with an LFO); here a
 * step per block, the dry level in its whole steps. Without an offset and with B 99 at 0 the same bits */
static B2_LATE void part_pre(sy_synth *s, float *vl, float *vr)   /* (sy_render, part_mix: once a block) */
{
    int i = (int)s->patch[B_OUT2] + ((s->dry_off + 128) >> 8);   /* (the dry level in whole steps) */
    float g;
    s->dry_g = HYB_OUT2[i < 0 ? 0 : i > 127 ? 127 : i];   /* B 58 a dry-path gain, not an output switch */
    if (!(s->patch[PB + 99] | s->rm_off)) return;
    g = sy_clampf((float)s->patch[PB + 99] + (float)s->rm_off * (1.f / 256.f), 0.f, 64.f);
    g = g <= 32.f ? 1.f - 0.293f * g * (1.f / 32.f) : 0.707f * (64.f - g) * (1.f / 32.f);
    NO_UNROLL for (i = 0; i < SY_BLOCK; i++) { vl[i] *= g; vr[i] *= g; }
}

/* input mode 2: the input through the filter section without a note (the static path) */
static void static_block(sy_synth *s, sy_vblk_t *b, float *m, float *l, float *r, const float *in)
{
    sy_voice_t *v = &s->stat;
    if (!v->on) {                       /* its LFOs start from the part's (free-running) phases */
        int i;
        for (i = 0; i < 3; i++) v->lfo[i] = s->glfo[i];
    }
    v->on = 1; v->gate = 1; v->vel = 100; v->key = 60; v->glide = v->target = 60.f;
    v->iph = s->patch[B_INIT_PH];
#ifdef SY_UNI_CAP
    v->ug = 1.f; v->ufb = 0;
#endif
    v->aenv.stage = 3; v->aenv.lvl = v->aenv.prev = 1.f;
    v->fenv.stage = 3; v->fenv.lvl = v->fenv.prev = 0.f;
    SY_FAR(voice_block)(s, v, b, m, 0);
    v->aenv.stage = 3; v->aenv.lvl = 1.f;
    b->use_input = 1;
    b->in_g /= 0.607f;                              /* (the mode-1 loss does not apply here) */
    b->m4_g = 0.0963f;                              /* mode 4: -24.9 dB through the static path */
    b->m1 = s->patch[F1_MODE] & 7;                  /* (mode 6 the bypass here: F1M6_DIST is a voice's; ASSUMPTION) */
    b->f1x = 0.f;
    b->gle *= 1.207f * 0.86f;                       /* the static path: -3.8 dB with the part's route (filt.json open; the
                                                     * impulse's peak reads -0.59, sat.json) */
    b->gre *= 1.207f * 0.86f;
    sy_voice_render(s, v, b, l, r, in);
}

int sy_voices_active(const sy_synth *s)
{
    int i, n = 0;
    for (i = 0; i < SY_NV(s); i++) n += s->v[i].on;
    return n;
}

/* (after 1.18C30) the overload guard's (a firmware's: app.c grd_*): n voices faded out over nb blocks (uni_fade: straight
 * in power, no click; a held note's other unison voices take over its level), the released ones first (the tails,
 * quietest first), then the held ones (quietest first: a note's unison voices before whole notes, as they are the
 * quietest of a note); voices already fading are not taken. Between two blocks. Returns the voices taken.
 * SY_SHED_KEEP 1 (after 1.18C34): a held note keeps one voice: of the held voices only a unison voice of a note that
 * still has two or more is taken, from the note with the most first (the quietest of its voices). Before (0) the
 * quietest held voice of all was taken: with two notes of two voices each, the older note in its decay was the
 * quieter, both its voices went and the note was cut as the second key sounded (a test patch, UNISON MAX 2, on the
 * device). A note's last voice is never shed here: the guard's other means (the reverb HALF, the governor's limit for
 * the next notes) take the rest */
#ifndef SY_SHED_KEEP
#define SY_SHED_KEEP 1
#endif
#if SY_SHED_KEEP && defined(SY_UNI_CAP)
static int shed_live(const sy_synth *s, int key)   /* a held note's voices sounding and not fading */
{
    int i, c = 0;
    for (i = 0; i < SY_NV(s); i++) c += s->v[i].on && s->v[i].gate && !s->v[i].ufade && s->v[i].key == key;
    return c;
}
#endif
SY_COLD int sy_shed(sy_synth *s, int n, int nb)
{
#ifdef SY_UNI_CAP
    int k;
    for (k = 0; k < n; k++) {
        int i, best = -1, rel = 0;
        float bl = 1e9f;
#if SY_SHED_KEEP
        int sh_c = 0;
#endif
        for (i = 0; i < SY_NV(s); i++) {
            const sy_voice_t *v = &s->v[i];
            const int r = !v->gate;
            if (!v->on || v->ufade) continue;
#if SY_SHED_KEEP
            if (!r) {                   /* (held: a unison voice of the note with the most voices, its quietest) */
                const int c = shed_live(s, v->key);
                if (c < 2 || rel) continue;
                if (best < 0 || c > sh_c || (c == sh_c && v->aenv.lvl * v->ug < bl)) { sh_c = c; bl = v->aenv.lvl * v->ug; best = i; }
                continue;
            }
#endif
            if (best < 0 || r > rel || (r == rel && v->aenv.lvl * v->ug < bl)) { rel = r; bl = v->aenv.lvl * v->ug; best = i; }
        }
        if (best < 0) break;
        uni_fade(s, &s->v[best], nb < 1 ? 1 : nb, 1);
    }
    return k;
#else
    (void)s; (void)n; (void)nb;
    return 0;
#endif
}

SY_COLD int sy_note_voices(const sy_synth *s)
{
    int n = s->patch[UNISON] + 1;
#ifdef SY_UNI_CAP
    const int lim = mono_mode(s) ? s->uni_mono : s->uni_max;
    if (lim && n > lim) n = lim;
#endif
    return n < 1 ? 1 : n;
}

SY_WORK_ATTR sy_work_t sy_work;

#if defined(SY_FX_PIPE) && SY_CORES < 2
#undef SY_FX_PIPE                       /* (two cores only) */
#endif
#if SY_CORES > 1
/* the voices on two cores: the control blocks first, the first half of the voices on this core and the rest there
 * (what one reads of the others decided before, sy_render, and the shared table built after); then the
 * second core renders the second half of the voices, each into its own buffers, while this one renders the first
 * half into the sum; their buffers are added after, in the voices' order, so the sum is the one-core sum */
static int (*cpu2_run)(void (*fn)(uint32_t), uint32_t arg);
static int (*cpu2_wait)(void);
void sy_set_cpu2(int (*run)(void (*fn)(uint32_t), uint32_t arg), int (*wait)(void)) { cpu2_run = run; cpu2_wait = wait; }
static sy_synth *job_s;
static const float *job_in;
static int job_idx[SY_MAX_VOICES], job_n0, job_n;
SY_WORK_ATTR static sy_vblk_t job_b[SY_MAX_VOICES];
#define JOB_MAX (SY_MAX_VOICES / 2)

/* the split of the voices between the cores, from the last blocks' times (SY_PROF; without a clock, half): the other
 * core renders its voices while this one renders its own, and nothing else of this core's overlaps (the control
 * blocks come before, the effects after); n0 (this core's voices) makes the two renders as long as each other:
 * n0 v0 = (nv - n0) v2. v0 is a voice's ticks here (the voices' time over their count), v2 a voice's ticks there,
 * known only when this core waited for it (the wait plus this core's voices, over its count; else taken as v0).
 * The sum stays in the voices' order whatever the split. */
#ifdef SY_PROF
extern uint32_t (*sy_prof_clock)(void);
extern uint32_t sy_prof[7];             /* (defined below, with the marks) */
#endif
static int split_n0_last, split_nv_last;
static float split_v0, split_v2;
#ifdef SY_PROF
uint32_t sy_split[2];                   /* (for a firmware's screen: this core's voices, all the voices) */
#endif
#ifndef SY_SPLIT_FIX
#define SY_SPLIT_FIX 0                  /* (1.18C49, a test build's) 1: the voices split between the cores by
                                         * their count alone (half on each), not by the measured times (split_n0's
                                         * estimate, SY_LPT): the output then does not depend on how fast the code
                                         * runs. The two cores' voices are summed in the split's order (this core's
                                         * into the sum, then the other's buffers), so two builds whose code runs at
                                         * other speeds may round a sum differently (+-1 LSB) wherever their splits
                                         * differ; with SY_SPLIT_FIX they cannot. 0: as shipped */
#endif
SY_BLKR static int split_n0(int nv)
{
#if defined(SY_PROF) && !SY_SPLIT_FIX
    int n0, lo = nv > JOB_MAX ? nv - JOB_MAX : 0;
    if (sy_prof_clock && split_n0_last > 0)
        split_v0 += ((float)sy_prof[2] / (float)split_n0_last - split_v0) * 0.125f;
    if (sy_prof_clock && split_nv_last > split_n0_last) {
        float v2 = (float)(sy_prof[2] + sy_prof[3]) / (float)(split_nv_last - split_n0_last);
        if (sy_prof[3] * 8u > sy_prof[2]) split_v2 += (v2 - split_v2) * 0.125f;   /* (measured: the other core was the
                                         * longer by more than the hand-over's own few ticks) */
        else {                                                          /* (it was not: at most this; the estimate
                                         * eases back towards this core's own, a core like it, until a wait says more) */
            float to = v2 < split_v0 ? v2 : split_v0;
            split_v2 += (to - split_v2) * 0.125f;
        }
    }
    if (split_v0 <= 0.f) n0 = nv - nv / 2;
    else {
        float v2 = split_v2 > 0.f ? split_v2 : split_v0;
        n0 = (int)((float)nv * v2 / (split_v0 + v2) + 0.5f);
    }
    if (n0 < lo) n0 = lo;
    if (n0 > nv) n0 = nv;
    return n0;
#else
    return nv - nv / 2;
#endif
}
/* (CLEANER, 1.18C25) the split by each voice's own time (SY_PROF): voices differ (the own oscillator's ring with a sine
 * costs a few times a plain saw's), so a split by their count leaves one core waiting for the other. Each voice's last
 * render (either core, lpt_cost) and the second core's own work besides its voices (its effects, lpt_fx); the voices
 * dearest first, each to the core that is done sooner (at most JOB_MAX there); job_idx then holds this core's voices
 * first, each core's in the voices' order. -1: no time known yet (split_n0's count) */
#if defined(SY_PROF) && !defined(PLAT_HOST) && !SY_SPLIT_FIX
#define SY_LPT 1                        /* (not on a host: its clock is the computer's, the sum's order from it would vary) */
static uint32_t lpt_cost[SY_MAX_VOICES], lpt_fx;
SY_BLKR static int split_lpt(int nv)
{
    int ord[SY_MAX_VOICES], on2[SY_MAX_VOICES], i, j, n0 = 0, n2 = 0, known = 0;
    uint32_t sum = 0u, l1 = 0u, l2 = lpt_fx, avg;
    if (!sy_prof_clock || nv < 2) return -1;
    for (i = 0; i < nv; i++) if (lpt_cost[job_idx[i]]) { known++; sum += lpt_cost[job_idx[i]]; }
    if (!known) return -1;
    avg = sum / (uint32_t)known;
    for (i = 0; i < nv; i++) {          /* (the dearest first: an insertion sort of at most 16) */
        uint32_t c = lpt_cost[job_idx[i]] ? lpt_cost[job_idx[i]] : avg;
        for (j = i; j > 0 && (lpt_cost[job_idx[ord[j - 1]]] ? lpt_cost[job_idx[ord[j - 1]]] : avg) < c; j--) ord[j] = ord[j - 1];
        ord[j] = i;
    }
    for (i = 0; i < nv; i++) {
        uint32_t c = lpt_cost[job_idx[ord[i]]] ? lpt_cost[job_idx[ord[i]]] : avg;
        if (l2 < l1 && n2 < JOB_MAX) { on2[ord[i]] = 1; l2 += c; n2++; }
        else { on2[ord[i]] = 0; l1 += c; }
    }
    {
        int t[SY_MAX_VOICES], k = 0;
        for (i = 0; i < nv; i++) if (!on2[i]) t[k++] = job_idx[i];
        n0 = k;
        for (i = 0; i < nv; i++) if (on2[i]) t[k++] = job_idx[i];
        for (i = 0; i < nv; i++) job_idx[i] = t[i];
    }
    return n0;
}
#define LPT_T0() uint32_t lpt_t = sy_prof_clock ? sy_prof_clock() : 0u
#define LPT_V(g) do { if (sy_prof_clock) { uint32_t t_ = sy_prof_clock(); lpt_cost[g] = (t_ - lpt_t) | 1u; lpt_t = t_; } } while (0)
#define LPT_FX(t0) do { if (sy_prof_clock) lpt_fx = sy_prof_clock() - (t0); } while (0)
#else
#define LPT_T0() (void)0
#define LPT_V(g) (void)0
#define LPT_FX(t0) (void)0
#endif
static void split_ran(int n0, int nv)
{
    split_n0_last = n0; split_nv_last = nv;
#ifdef SY_PROF
    sy_split[0] = (uint32_t)n0; sy_split[1] = (uint32_t)nv;
#endif
}
SY_WORK_ATTR static float job_out[JOB_MAX][2][SY_BLOCK];   /* (the second core's share: at most half) */
/* the control blocks: the voices' first part on this core, the rest there (each with its own matrix buffer) */
static int ctl_idx[SY_MAX_VOICES], ctl_n0, ctl_n, ctl_newest;
static sy_synth *ctl_s;
SY_WORK_ATTR static float ctl_m[256];
SY_CTL static void ctl_one(sy_synth *s, int i, float *m, int newest)
{
    sy_voice_t *v = &s->v[i];
    SY_FAR(voice_block)(s, v, &job_b[i], m, i == newest);
    if (ARP_KEEP_PREV) {
        uint32_t *h = v->hph[s->block & 3];
        h[0] = v->ph1; h[1] = v->ph2; h[2] = v->ph3; h[3] = v->phs;
    }
}
SY_CTL static void ctl_job(uint32_t arg)
{
    int j;
    (void)arg;
    for (j = ctl_n0; j < ctl_n; j++) SY_FAR(ctl_one)(ctl_s, ctl_idx[j], ctl_m, ctl_newest);
}

#ifdef SY_FX_PIPE
/* the effects a block later, on the second core (SY_FX_PIPE, a firmware's choice: not the reference's timing): while
 * this core renders its voices, that one renders its voices and then the effects of the previous block's sum; the
 * output is the one-core output one block later, except that a parameter's change reaches the effects a block early
 * (the effects of block n read the parameters of block n+1). The state they read and this core writes (the gains,
 * the tempo) is written after the wait. */
SY_WORK_ATTR static float pipe_l[SY_BLOCK], pipe_r[SY_BLOCK];   /* the previous block's sum */
static uint8_t pipe_z = 1;              /* (part A's: pipe_l, pipe_r hold zeros) */
static float *job_ol, *job_or;           /* the effects' output: the caller's buffers */
static int job_fx;
#endif
#ifdef SY_HOT_RAM
__attribute__((section(".ram_text")))
#endif
static void voices_job(uint32_t arg)    /* the second core's half */
{
    int j, k;
    LPT_T0();
    (void)arg;
    for (j = job_n0; j < job_n; j++) {
        float *ol = job_out[j - job_n0][0], *orr = job_out[j - job_n0][1];
        for (k = 0; k < SY_BLOCK; k++) ol[k] = orr[k] = 0.f;
        sy_voice_render_w(job_s, &job_s->v[job_idx[j]], &job_b[job_idx[j]], ol, orr, job_in, 1);
        LPT_V(job_idx[j]);
    }
#ifdef SY_FX_PIPE
    if (job_fx) SY_FAR(sy_fx_render)(job_s, job_ol, job_or, pipe_l, pipe_r);
    LPT_FX(lpt_t);
    job_fx = 0;
#endif
}
#else
void sy_set_cpu2(int (*run)(void (*fn)(uint32_t), uint32_t arg), int (*wait)(void)) { (void)run; (void)wait; }
#endif

/* the block's time by part (SY_PROF, a firmware's measurement): sy_prof_clock set by the firmware; sy_prof holds the last
 * block's ticks: 0 events and LFOs, 1 the voices' control blocks, 2 the voices here, 3 the wait for the second core,
 * 4 its voices added, 5 the effects, 6 the rest */
#ifdef SY_PROF
uint32_t (*sy_prof_clock)(void);
uint32_t sy_prof[7], sy_prof_v[3];
static uint32_t prof_t;
#define PROF(i) do { if (sy_prof_clock) { uint32_t t_ = sy_prof_clock(); sy_prof[i] = t_ - prof_t; prof_t = t_; } } while (0)
#define PROF0() do { if (sy_prof_clock) prof_t = sy_prof_clock(); sy_prof_v[0] = sy_prof_v[1] = sy_prof_v[2] = 0u; } while (0)
#else
#define PROF(i) ((void)0)
#define PROF0() ((void)0)
#endif

/* the engine's synth size, for a firmware's check that its own sy_synth (synth.h with its SY_MAX_VOICES) matches */
const unsigned sy_synth_size = sizeof(sy_synth);

static int clip_in_render = 1;          /* (0 while sy_render_i24 does the clip itself, with the scaling) */
static uint8_t out_zero;                /* the last block's output: zeros, known (the effects at rest; sy_render_i24) */
/* the effects at rest (sy_fx.c SY_FX_REST): part A's / the single engine's chain on its input (z: all 0), skipped
 * when it rests. HYB_MULTI: part A's space takes B's send, at rest only while that is held at 0 (sy_fx_quiet), B
 * sends nothing or A has no space */
#if SY_FX_REST
#ifdef HYB_MULTI
#define FXR_ZIN(s, z) ((z) && (!(s)->dr_on || !(s)->patch[112]) && (!(s)->mx_on || !(s)->patch[112] || (mx_m && mx_m->p[1]->mx_bz)))
#else
#define FXR_ZIN(s, z) ((z) && (!(s)->dr_on || !(s)->patch[112]))   /* (the drums' signal into the space,
                                         * synth.h dr_on) */
#endif
#define FX_BEGIN(s, z) sy_fx_rest_begin((s), FXR_ZIN((s), (z)))
#define FX_STEP(s, l, r) sy_fx_rest_step((s), (l), (r))
#define FX_END(s, l, r) sy_fx_rest_end((s), (l), (r))
#else
#define FX_BEGIN(s, z) 0
#define FX_STEP(s, l, r) ((void)0)
#define FX_END(s, l, r) ((void)0)
#endif
void sy_render(sy_synth *s, float *out_l, float *out_r, const float *in_l, const float *in_r)
{
    SY_WORK_ATTR static float m[256];    /* (all 0 between blocks: voice_block clears the slots it touched) */
    float *restrict vl = sy_work.vl, *restrict vr = sy_work.vr, *restrict insel = sy_work.in;
    const float *in = 0;
    sy_vblk_t b;
    int i, newest = -1, in_mode = s->patch[IN_MODE];
    uint32_t na = 0;
    int fx_rest = 0;                    /* (the effects at rest this block: skipped) */
#ifdef SY_FX_PIPE
    int fx_done = 0, pipe = SY_PIPE_ON(s);
    static int pipe_was;
    if (pipe != pipe_was) {             /* (switched: the block in the pipe is dropped, a 2.9 ms gap once) */
        for (i = 0; i < SY_BLOCK; i++) pipe_l[i] = pipe_r[i] = 0.f;
        pipe_was = pipe;
        pipe_z = 1;
    }
#endif
    PROF0();
#if SY_CORES > 1
    if (!cpu2_run || in_mode == 2)      /* (two cores: the voices' blocks are job_b, this one the static path's only) */
#endif
    {   /* every field defined, also those a voice's path leaves unset (a byte loop: no memset in the freestanding builds) */
        volatile unsigned char *pz = (volatile unsigned char *)&b;
        unsigned k;
        for (k = 0; k < sizeof b; k++) pz[k] = 0;
    }

#include "sy_part_head.inc"
    if (s->fx_tail) SY_FAR(fx_tail)(s);   /* (the effects' matrix offsets after the last voice ended) */

    PROF(0);
    for (i = 0; i < SY_BLOCK; i++) vl[i] = vr[i] = 0.f;
    SY_FAR(blk_cache)(s);                    /* (the shared values of the voices' blocks: before both cores read them) */
    s->n_on = 0;

    for (i = 0; i < SY_NV(s); i++) {
        if (s->v[i].on && (newest < 0 || s->v[i].age > s->v[newest].age)) newest = i;
        s->n_on += s->v[i].on;
    }
    s->v6 = (uint8_t)SY_FAR(v6_on)(s);
#if SY_CORES > 1
    if (cpu2_run) {
        int nv = 0, j, k, two, nc = 0, par = 1;
        for (i = 0; i < SY_NV(s); i++) if (s->v[i].on) ctl_idx[nc++] = i;
#ifdef CLN_LFO_TAB
        for (j = 0; j < nc && par; j++) {   /* a voice's first block: "another voice sounds", as the one-core order has it
                                         * (a voice before it in the order may end in its own block: then one core) */
            int a = ctl_idx[j], hi = 0, lo = 0;
            if (!s->v[a].lfo_pend) continue;
            for (k = 0; k < s->max_voices; k++)
                if (k != a && s->v[k].on) { if (k > a) hi = 1; else lo = 1; }
            if (hi || !lo) ctl_oth[a] = (uint8_t)hi; else par = 0;
        }
#endif
        ctl_s = s; ctl_newest = newest; ctl_n = nc; ctl_n0 = nc - nc / 2;
        two = par && nc >= 2 && (ctl_par = 1, cpu2_run(ctl_job, 0u));
        if (!two) ctl_n0 = nc;
        for (j = 0; j < ctl_n0; j++) SY_FAR(ctl_one)(s, ctl_idx[j], m, newest);
        if (two && cpu2_wait() != 0) SY_FAR(ctl_job)(0u);   /* (it was given up: its half here) */
        if (ctl_par) {                   /* the saturation's table, as the last voice in the order built it */
            int t = s->patch[SAT_TYPE];
            ctl_par = 0;
            if (t >= 1 && t <= 4)
                for (j = 0; j < nc; j++) {
                    float drv = job_b[ctl_idx[j]].sat_w;
                    if (!(drv >= 0.f && drv <= 1.f)) continue;   /* (a value from the other core out of its range: a
                                         * misread, which some FM-1s' second core makes; not built from) */
                    if (s->sat_type_built != t || s->sat_drive_built != drv) {
                        SY_FAR(sy_sat_build)(s, t, drv);
                        s->sat_type_built = (uint8_t)t;
                        s->sat_drive_built = drv;
                    }
                }
        }
        for (j = 0; j < nc; j++) {
            sy_voice_t *v = &s->v[ctl_idx[j]];
            if (v->on || v->aenv.prev > 0.f) job_idx[nv++] = ctl_idx[j];
            else v->fresh = 0;
            na++;
        }
        PROF(1);
        job_s = s; job_in = in; job_n = nv; job_n0 = SY_BLKC(split_n0)(nv);
#ifdef SY_LPT
        { const int n0 = SY_BLKC(split_lpt)(nv); if (n0 >= 0) job_n0 = n0; }
#endif
#ifdef SY_FX_PIPE
        if (pipe && (fx_rest = FX_BEGIN(s, pipe_z)) != 0) {   /* (the effects at rest: skipped here, before the
                                         * voices, as the other core would have run them: on the last block's state) */
            FX_STEP(s, out_l, out_r);
            fx_done = 1;
            job_fx = 0;
            two = job_n0 < nv && cpu2_run(voices_job, 0u);   /* (no voices: nothing handed out) */
        } else if (pipe) {
            job_ol = out_l; job_or = out_r; job_fx = 1;
            two = cpu2_run(voices_job, 0u);   /* (also without voices there: the effects) */
            fx_done = two;               /* (there, or here if it was given up) */
            if (!two) job_fx = 0;
        } else {
            job_fx = 0;
            two = job_n0 < nv && cpu2_run(voices_job, 0u);
        }
#else
        two = job_n0 < nv && cpu2_run(voices_job, 0u);
#endif
        split_ran(two ? job_n0 : nv, nv);
        {
            LPT_T0();
            for (j = 0; j < (two ? job_n0 : nv); j++) {
                SY_FAR(sy_voice_render_w)(s, &s->v[job_idx[j]], &job_b[job_idx[j]], vl, vr, in, 0);
                LPT_V(job_idx[j]);
            }
        }
        PROF(2);
        if (two && cpu2_wait() != 0) SY_FAR(voices_job)(0u);   /* (it was given up: its half here) */
        PROF(3);
        if (two)
            for (j = job_n0; j < nv; j++)
                for (k = 0; k < SY_BLOCK; k++) { vl[k] += job_out[j - job_n0][0][k]; vr[k] += job_out[j - job_n0][1][k]; }
        for (j = 0; j < nv; j++) s->v[job_idx[j]].fresh = 0;
    } else
#endif
    for (i = 0; i < SY_NV(s); i++) {
        sy_voice_t *v = &s->v[i];
        if (!v->on) continue;
        SY_FAR(voice_block)(s, v, &b, m, i == newest);
        if (ARP_KEEP_PREV) {                /* (a short history of the phases, for the arp's hand-over) */
            uint32_t *h = v->hph[s->block & 3];
            h[0] = v->ph1; h[1] = v->ph2; h[2] = v->ph3; h[3] = v->phs;
        }
        if (v->on || v->aenv.prev > 0.f) sy_voice_render(s, v, &b, vl, vr, in);
        v->fresh = 0;
        na++;
    }
    if (in_mode == 2) static_block(s, &b, m, vl, vr, in);
    s->pv_jump = 0;
    s->part_snd = na > 0 || in_mode >= 1;          /* the part sounds (synth.h) */

    /* the part: second output (B 58) takes it off the main output; the effects; the input straight to them */
    SY_FAR(part_pre)(s, vl, vr);        /* the dry level (B 58), the input ring modulator on the voices (B 99) */
    if (in_mode == 3) for (i = 0; i < SY_BLOCK; i++) { vl[i] += in[i]; vr[i] += in[i]; }
    s->part_g = hyb_tab(HYB_PATCH_G, s->pv[PATCH_VOL]) * hyb_tab(HYB_CC7_G, s->pv[PART_VOL]);
#ifdef HYB_MULTI
    s->part_g *= s->rt_g;               /* (the player's part volume: after the distortion, before the chorus and
                                         * the delay / reverb send) */
#endif
    s->bpm_now = tempo_bpm(s);
    PROF(4);                             /* (two cores: their voices added; one core: the voices, 1-3 stay 0) */
    {
        const int zs = na == 0 && in_mode == 0;   /* (vl, vr hold zeros: no voice, no input route) */
#ifdef SY_FX_PIPE
        if (pipe) {
            if (!fx_rest) {
                if (!fx_done) SY_FAR(sy_fx_render)(s, out_l, out_r, pipe_l, pipe_r);
                FX_END(s, out_l, out_r);
            }
            if (!(zs && pipe_z)) for (i = 0; i < SY_BLOCK; i++) { pipe_l[i] = vl[i]; pipe_r[i] = vr[i]; }
            pipe_z = (uint8_t)zs;
        } else
#endif
        if ((fx_rest = FX_BEGIN(s, zs)) != 0) FX_STEP(s, out_l, out_r);
        else {
            SY_FAR(sy_fx_render)(s, out_l, out_r, vl, vr);
            FX_END(s, out_l, out_r);
        }
    }
    out_zero = (uint8_t)fx_rest;
    PROF(5);
    if (clip_in_render)
    for (i = 0; i < SY_BLOCK; i++) {               /* the output: a hard clip at full scale */
        float l = out_l[i], r = out_r[i];
        if (s->opt & SY_OPT_SOFTCLIP) { l = sy_tanh(l); r = sy_tanh(r); }
        out_l[i] = sy_clampf(l, -1.f, 1.f);
        out_r[i] = sy_clampf(r, -1.f, 1.f);
    }
    s->xclk_frames += (float)SY_BLOCK;
    if (s->xclk_on && s->xclk_frames > s->fs) { s->xclk_on = 0; s->xclk_per = 0.f; }   /* no tick for 1 s: off */
    (void)prng_draw(s);                 /* CLEANER: the phases' generator runs on once a block (a fresh voice's phases
                                         * depend on when it starts, as on the development engine: ph_meas.py, bank_stat.py) */
#if DET_WANDER
    SY_FAR(dw_tick)(s);                 /* CLEANER: the detune's wanders (the next block's) */
#endif
    s->block++;
    (void)na;
    PROF(6);
}

/* ---- an integer interface, for callers built without floating point (the FM-1 app is; only the engine's own
 * sources get the FPU flags) */
void sy_init_hz(sy_synth *s, uint32_t hz) { sy_init(s, (float)hz); }

/* the block clipped and scaled to interleaved int32 frames in one pass (a firmware's 24-bit output: g = full scale);
 * the same operations as sy_render's clip and then (int32_t)(x * g). The pass in RAM (SY_HOT_RAM: a cold line of
 * the FM-1's flash costs ~500 cycles), called far */
#ifdef SY_HOT_RAM
__attribute__((section(".ram_text"), noinline))
#endif
static void i24_out(int32_t *lr, const float *l, const float *r, float g, int soft)
{
    int i;
    if (soft)
        for (i = 0; i < SY_BLOCK; i++) {
            lr[2 * i] = (int32_t)(sy_clampf(sy_tanh(l[i]), -1.f, 1.f) * g);
            lr[2 * i + 1] = (int32_t)(sy_clampf(sy_tanh(r[i]), -1.f, 1.f) * g);
        }
    else
        for (i = 0; i < SY_BLOCK; i++) {
            lr[2 * i] = (int32_t)(sy_clampf(l[i], -1.f, 1.f) * g);
            lr[2 * i + 1] = (int32_t)(sy_clampf(r[i], -1.f, 1.f) * g);
        }
}
int sy_render_i24(sy_synth *s, int32_t *lr, float g)
{
    float *restrict l = sy_work.l, *restrict r = sy_work.r;
    int i;
    clip_in_render = 0;
    sy_render(s, l, r, 0, 0);
    clip_in_render = 1;
    if (out_zero) {                     /* (the effects at rest: zeros, known) */
        for (i = 0; i < 2 * SY_BLOCK; i++) lr[i] = 0;
        return 1;
    }
    SY_FAR(i24_out)(lr, l, r, g, (s->opt & SY_OPT_SOFTCLIP) != 0);
    return 0;
}

void sy_render_s16(sy_synth *s, uint32_t *frames, const uint32_t *in)
{
    float *restrict l = sy_work.l, *restrict r = sy_work.r, *restrict il = sy_work.il, *restrict ir = sy_work.ir;
    int i;
    if (in)
        for (i = 0; i < SY_BLOCK; i++) {
            il[i] = (float)(int16_t)(in[i] & 0xFFFFu) * (1.f / 32768.f);
            ir[i] = (float)(int16_t)(in[i] >> 16) * (1.f / 32768.f);
        }
    sy_render(s, l, r, in ? il : 0, in ? ir : 0);
    for (i = 0; i < SY_BLOCK; i++) {    /* full scale 1.0 -> +-32767; L in the low half, R in the high half */
        int32_t a = (int32_t)(l[i] * 32767.f), b = (int32_t)(r[i] * 32767.f);
        frames[i] = (uint32_t)(uint16_t)(int16_t)a | (uint32_t)(uint16_t)(int16_t)b << 16;
    }
}

#ifdef HYB_MULTI
/* ---- two parts (HYB_MULTI, synth.h sy_multi_t). SINGLE is sy_render on part A, its slice the
 * whole pool. LAYER, SPLIT and CHANNELS render both parts in one pass: each part's events, arpeggiator, smoothing and
 * LFOs (sy_part_head.inc), then the voices of both on the two cores as sy_render shares them (the control blocks,
 * then the voices; each voice with its part's control), then the effects: part B's (its send into A's space kept),
 * then part A's with B's send, the sum. */
static int mx_fresh;                    /* the setup changed (render2: the effects' pipe emptied) */
SY_MX_ATTR static float mx_vl[SY_BLOCK], mx_vr[SY_BLOCK];   /* part B's voices' sum (A's: sy_work.vl / vr) */
SY_MX_ATTR static float mx_in[SY_BLOCK];                    /* part B's input select (no input: 0) */
SY_MX_ATTR static float mx_b[2][SY_BLOCK];                  /* part B's send (sy_fx.c) */
SY_MX_ATTR static float mx_o[2][SY_BLOCK];                  /* part B's effects' output */
SY_MX_ATTR static float mx_mm[256];                         /* (all 0 between blocks, as sy_render's m) */
#define MX_PART(g) ((g) >= mx_m->p[1]->v0)   /* a pool voice's part (A's slice first) */

static const float *part_head(sy_synth *s, const float *in_l, const float *in_r, int in_mode, float *insel)
{
    const float *in = 0;
    int i;
#include "sy_part_head.inc"
    if (s->fx_tail) SY_FAR(fx_tail)(s);   /* (as sy_render) */
    return in;
}
static void part_mix(sy_synth *s, float *vl, float *vr, const float *in, int in_mode)   /* (as sy_render after its
                                         * voices) */
{
    int i;
    SY_FAR(part_pre)(s, vl, vr);
    if (in_mode == 3) for (i = 0; i < SY_BLOCK; i++) { vl[i] += in[i]; vr[i] += in[i]; }
    s->part_g = hyb_tab(HYB_PATCH_G, s->pv[PATCH_VOL]) * hyb_tab(HYB_CC7_G, s->pv[PART_VOL]);
#ifdef HYB_MULTI
    s->part_g *= s->rt_g;               /* (the player's part volume: after the distortion, before the chorus and
                                         * the delay / reverb send) */
#endif
    s->bpm_now = tempo_bpm(s);
}
static void part_post2(sy_synth *s)     /* (as sy_render's end) */
{
    s->xclk_frames += (float)SY_BLOCK;
    if (s->xclk_on && s->xclk_frames > s->fs) { s->xclk_on = 0; s->xclk_per = 0.f; }
    (void)prng_draw(s);                 /* (as sy_render's end) */
#if DET_WANDER
    SY_FAR(dw_tick)(s);
#endif
    s->block++;
}
/* part B at rest (multi.md 6a): while B has no voice, no input route and no patch change, and had none the block
 * before (the effects' pipe takes the voices a block later), its effects take zeros; once they changed nothing for
 * MX_Q_HOLD blocks but tails below -180 dB (sy_fx_still, sy_fx_buf_still), render2 skips them (sy_fx_quiet: only
 * B's path to A's delay / reverb runs on while A's takes B's send) until a voice, an input route, a patch byte or a
 * new setup comes: the effects then go on from the states they had */
#define MX_Q_HOLD 16
static uint8_t mx_skip, mx_cand;        /* this block: B's effects skipped; run and checked */
static uint8_t mx_rest;                 /* B rests (from the next block) */
static uint8_t mx_qprev;                /* B was quiet the block before */
static uint16_t mx_qn;                  /* blocks its effects changed nothing */
/* both parts' effects: B's first (its send), then A's with it, B's output added (za: A's input all 0; A's chain at
 * rest skipped, as the single engine's, sy_render) */
static uint8_t fx2_zero;                /* (the last fx2's output: zeros, known) */
static void fx2(float *l, float *r, const float *al, const float *ar, const float *bl, const float *br, int za)
{
    sy_synth *a = mx_m->p[0], *b = mx_m->p[1];
    int i, ra;
    if (mx_skip) {
#ifdef MX_Q_DEBUG
        { extern unsigned mx_q_dbg; mx_q_dbg++; }   /* (a host test's count of the blocks skipped) */
#endif
        sy_fx_quiet(b, a->patch[112] != 0, mx_o[0], mx_o[1]);
    } else {
        b->mx_bz = 0;
        if (mx_cand) sy_fx_snap(b);
        SY_FAR(sy_fx_render)(b, mx_o[0], mx_o[1], bl, br);
        if (mx_cand) {
            if (!sy_fx_still(b, mx_o[0], mx_o[1], b->mx_bl, b->mx_br)) mx_qn = 0;
            else if (++mx_qn >= MX_Q_HOLD) {
                if (sy_fx_buf_still(b)) mx_rest = 1;
                mx_qn = 0;
            }
        }
    }
    a->mx_wb = b->mx_wb;
    a->mx_on = b->mx_wb > 0.f;
    if ((ra = FX_BEGIN(a, za)) != 0) FX_STEP(a, l, r);
    else {
        SY_FAR(sy_fx_render)(a, l, r, al, ar);
        FX_END(a, l, r);
    }
    if (!mx_skip) for (i = 0; i < SY_BLOCK; i++) { l[i] += mx_o[0][i]; r[i] += mx_o[1][i]; }
    fx2_zero = (uint8_t)(ra && mx_skip);
}

/* the setup taken at a block's start: a new mode or split of the voices stops every voice (the slices move), the
 * parts' held keys forgotten; the voice limit split between the parts by their slices */
static void multi_setup(sy_multi_t *m)
{
    sy_synth *a = m->p[0], *b = m->p[1];
    int mode = m->mode < SY_M_N ? m->mode : SY_M_SINGLE, na = m->nv_a, mv = m->max_voices, i;
    if (mode == SY_M_SINGLE) na = SY_MAX_VOICES;
    else if (na < 1) na = 1;
    else if (na > SY_MAX_VOICES - 1) na = SY_MAX_VOICES - 1;
    if (mode != m->mode_now || na != m->nv_now || (m->dyn != 0) != m->dyn_now) {
        for (i = 0; i < SY_MAX_VOICES; i++) {   /* (fresh voices, each seeded as its part's voice of that index:
                                         * a part renders as it would alone, sy_init) */
            zero(&m->sh->v[i], sizeof m->sh->v[i]);
            m->sh->v[i].rng = 0x9E3779B9u * (uint32_t)((i < na ? i : i - na) + 1);
        }
        a->v = m->sh->v; a->nv = na; a->v0 = 0;
        b->v = m->sh->v + na; b->nv = SY_MAX_VOICES - na; b->v0 = na;
        sy_all_off(a);
        sy_all_off(b);
        for (i = 0; i < 128; i++) m->kp[i] = 0;
#if SY_CORES > 1
        zero(job_b, sizeof job_b);      /* (the voices' block values: as at the start; a value a voice's path reads
                                         * before it sets it is its slot's last, synth.h HYB_MULTI notes) */
#endif
        m->mode_now = (uint8_t)mode;
        m->nv_now = (uint8_t)na;
        m->dyn_now = (uint8_t)(m->dyn != 0);
        mx_fresh = 1;
    }
    if (mv < 1) mv = 1;
    if (mv > SY_MAX_VOICES) mv = SY_MAX_VOICES;
    if (mode != SY_M_SINGLE && mv < 4) mv = 4;   /* (two parts: their guarantees) */
    m->cap_now = (int16_t)mv;
    a->uni_max = b->uni_max = m->uni_max;
    a->uni_mono = b->uni_mono = m->uni_mono;
    a->uni_auto = b->uni_auto = m->uni_auto;
    a->uni_steal = b->uni_steal = m->uni_steal;
    if (mode == SY_M_SINGLE) a->max_voices = mv;
    else if (m->dyn_now) { a->max_voices = a->nv; b->max_voices = b->nv; }   /* (the cap: mx_room) */
    else {
        int ma = (mv * na + SY_MAX_VOICES / 2) / SY_MAX_VOICES, mb;
        if (ma < 1) ma = 1;
        if (ma > na) ma = na;
        mb = mv - ma;
        if (mb < 1) mb = 1;
        if (mb > b->nv) mb = b->nv;
        a->max_voices = ma;
        b->max_voices = mb;
    }
    b->send = m->send_b;
    b->opt = a->opt;                    /* (B's options A's: the latency, the effects' pipe) */
}

SY_COLD void sy_multi_init(sy_multi_t *m, sy_synth *a, sy_synth *b, sy_shared_t *sh, float (*ch_a)[2048], float (*ch_b)[2048],
                   float *sat_b, float fs)
{
    int k, i;
    zero(m, sizeof *m);
    zero(sh, sizeof *sh);               /* (as sy_init clears the single engine's: the voices, the delay's memory) */
    m->p[0] = a; m->p[1] = b; m->sh = sh;
    for (k = 0; k < 2; k++) {
        sy_synth *s = m->p[k];
        s->v = sh->v + (k ? SY_MAX_VOICES : 0);
        s->nv = k ? 0 : SY_MAX_VOICES;
        s->v0 = k ? SY_MAX_VOICES : 0;
        s->pidx = (uint8_t)k;
        s->steal = sh->steal;
        s->sat_lut = k ? sat_b : sy_sat_lut;
        s->ch_buf = k ? ch_b : ch_a;
        s->tables = sh->tables;
        s->cmap = sh->cmap;
        s->dl_buf = sh->sp.d.dl_buf;
        s->rv_buf = 0;                  /* (none: sy_shared_t) */
        s->rvx_ring = sh->sp.rvx_ring;
        sy_init(s, fs);
        s->mx_bl = mx_b[0];
        s->mx_br = mx_b[1];
    }
    b->channel = -1;                    /* (the setup routes: sy_multi_midi) */
    for (i = 0; i < SY_MAX_VOICES; i++) sh->v[i].rng = 0x9E3779B9u * (uint32_t)(i + 1);   /* (as A's, SINGLE) */
    m->mode = m->mode_now = SY_M_SINGLE;
    m->nv_a = SY_MAX_VOICES / 2;
    m->nv_now = SY_MAX_VOICES;
    m->split = 60;
    m->ch_b = 1;
    m->send_b = -1;
    m->max_voices = SY_MAX_VOICES;
    mx_m = m;
}

static void multi_route(sy_multi_t *m, uint8_t st, uint8_t d1, uint8_t d2, int chb)
{
    sy_synth *a = m->p[0], *b = m->p[1];
    int ty = st & 0xF0, mode = m->mode, to;
    if (mode == SY_M_SINGLE || st == SY_EV_PARAM) { sy_midi(a, st, d1, d2); return; }   /* (a patch byte: A's; B's: sy_midi) */
    if (st >= 0xF0) { sy_midi(a, st, d1, d2); sy_midi(b, st, d1, d2); return; }   /* (the clock, transport: both) */
    if (chb && m->ch_b >= 0 && (st & 0x0F) == m->ch_b) { sy_midi(b, st, d1, d2); return; }   /* part B's channel: B alone */
    if (ty == 0x90 && d2) {
        to = mode == SY_M_LAYER ? 3 : mode == SY_M_SPLIT ? ((d1 & 127) < m->split ? 2 : 1) : 1;
        m->kp[d1 & 127] |= (uint8_t)to;
    } else if (ty == 0x80 || ty == 0x90) {
        to = m->kp[d1 & 127];
        m->kp[d1 & 127] = 0;
        if (!to) to = mode == SY_M_CHANNELS ? 1 : 3;   /* (a key not seen on: where it might be) */
    } else if (ty == 0xB0 && (d1 == 120 || d1 == 123)) to = 3;   /* (all sound / notes off: both, always) */
    else to = mode == SY_M_CHANNELS ? 1 : 3;      /* controllers, bend, pressure: both (CHANNELS: A) */
    if (to & 1) sy_midi(a, st, d1, d2);
    if (to & 2) sy_midi(b, st, d1, d2);
}
void sy_multi_midi(sy_multi_t *m, uint8_t st, uint8_t d1, uint8_t d2) { multi_route(m, st, d1, d2, 1); }
void sy_multi_play(sy_multi_t *m, uint8_t st, uint8_t d1, uint8_t d2) { multi_route(m, st, d1, d2, 0); }

int sy_multi_voices_active(const sy_multi_t *m)
{
    return sy_voices_active(m->p[0]) + (m->mode_now != SY_M_SINGLE ? sy_voices_active(m->p[1]) : 0);
}

#if SY_CORES > 1
static int mx_newest[2];
static const float *mx_jin[2];
#ifdef SY_FX_PIPE
SY_MX_ATTR static float pipe_bl[SY_BLOCK], pipe_br[SY_BLOCK];   /* part B's previous block (A's: pipe_l, pipe_r) */
static int mx_fx;
static float *mx_ol, *mx_or;
#endif
SY_CTL static void ctl2_one(int g, float *m)   /* a pool voice's control block (its part's) */
{
    sy_synth *s = mx_m->p[MX_PART(g)];
    sy_voice_t *v = &mx_m->sh->v[g];
    SY_FAR(voice_block)(s, v, &job_b[g], m, g - s->v0 == mx_newest[MX_PART(g)]);
    if (ARP_KEEP_PREV) {
        uint32_t *h = v->hph[s->block & 3];
        h[0] = v->ph1; h[1] = v->ph2; h[2] = v->ph3; h[3] = v->phs;
    }
}
SY_CTL static void ctl2_job(uint32_t arg)
{
    int j;
    (void)arg;
    for (j = ctl_n0; j < ctl_n; j++) SY_FAR(ctl2_one)(ctl_idx[j], ctl_m);
}
#ifdef SY_HOT_RAM
__attribute__((section(".ram_text")))
#endif
static void voices2_job(uint32_t arg)   /* the second core's voices (and, piped, both parts' effects) */
{
    int j, k;
    LPT_T0();
    (void)arg;
    for (j = job_n0; j < job_n; j++) {
        int g = job_idx[j], q = MX_PART(g);
        float *ol = job_out[j - job_n0][0], *orr = job_out[j - job_n0][1];
        for (k = 0; k < SY_BLOCK; k++) ol[k] = orr[k] = 0.f;
        sy_voice_render_w(mx_m->p[q], &mx_m->sh->v[g], &job_b[g], ol, orr, mx_jin[q], 1);
        LPT_V(g);
    }
#ifdef SY_FX_PIPE
    if (mx_fx) SY_FAR(fx2)(mx_ol, mx_or, pipe_l, pipe_r, pipe_bl, pipe_br, pipe_z);
    LPT_FX(lpt_t);
    mx_fx = 0;
#endif
}
#endif

static void render2(sy_multi_t *mm, float *out_l, float *out_r, const float *in_l, const float *in_r)
{
    sy_synth *P[2];
    float *vl[2], *vr[2];
    const float *in[2];
    int inm[2], newest[2] = {-1, -1}, i, k;
    uint32_t na[2] = {0u, 0u};
    sy_vblk_t b;
    float *m = mx_mm;
#ifdef SY_FX_PIPE
    int fx_done = 0, pipe;
    static int pipe_was = -1;
#endif
    int fresh = mx_fresh;
    P[0] = mm->p[0]; P[1] = mm->p[1];
    vl[0] = sy_work.vl; vr[0] = sy_work.vr; vl[1] = mx_vl; vr[1] = mx_vr;
#ifdef SY_FX_PIPE
    pipe = SY_PIPE_ON(P[0]);
    if (pipe != pipe_was || mx_fresh) { /* (switched, or a new setup: the blocks in the pipe dropped) */
        for (i = 0; i < SY_BLOCK; i++) pipe_l[i] = pipe_r[i] = pipe_bl[i] = pipe_br[i] = 0.f;
        pipe_was = pipe;
        pipe_z = 1;
    }
    mx_fresh = 0;
#endif
    PROF0();
    for (k = 0; k < 2; k++) inm[k] = P[k]->patch[IN_MODE];
    {
        volatile unsigned char *pz = (volatile unsigned char *)&b;
        unsigned q;
        for (q = 0; q < sizeof b; q++) pz[q] = 0;
    }
    for (k = 0; k < 2; k++) in[k] = part_head(P[k], k ? 0 : in_l, k ? 0 : in_r, inm[k], k ? mx_in : sy_work.in);
    PROF(0);
    for (k = 0; k < 2; k++) {
        sy_synth *s = P[k];
        for (i = 0; i < SY_BLOCK; i++) vl[k][i] = vr[k][i] = 0.f;
        SY_FAR(blk_cache)(s);
        s->n_on = 0;
        for (i = 0; i < s->nv; i++) {
            if (s->v[i].on && (newest[k] < 0 || s->v[i].age > s->v[newest[k]].age)) newest[k] = i;
            s->n_on += s->v[i].on;
        }
        s->v6 = (uint8_t)SY_FAR(v6_on)(s);
    }
    {   /* part B at rest (fx2): quiet this block and the one before (the effects' pipe takes the voices a block
         * later), else its effects run (and it rests no more); decided before the effects' job starts */
        sy_synth *b = P[1];
        int wake = b->fx_wake, quiet;
        b->fx_wake = 0;
        quiet = !b->n_on && !inm[1] && !b->sm_dirty && !wake && !fresh;
        if (!quiet || !mx_qprev) { mx_rest = 0; mx_qn = 0; mx_skip = mx_cand = 0; }
        else { mx_skip = mx_rest; mx_cand = !mx_rest; }
        mx_qprev = (uint8_t)quiet;
    }
#if SY_CORES > 1
    if (cpu2_run) {
        int nv = 0, j, q, two, nc = 0, par = 1;
        for (k = 0; k < 2; k++)
            for (i = 0; i < P[k]->nv; i++) if (P[k]->v[i].on) ctl_idx[nc++] = P[k]->v0 + i;
#ifdef CLN_LFO_TAB
        for (j = 0; j < nc && par; j++) {   /* (as sy_render: "another voice sounds", within the voice's part) */
            int g = ctl_idx[j], hi = 0, lo = 0;
            sy_synth *s = P[MX_PART(g)];
            int a = g - s->v0;
            if (!s->v[a].lfo_pend) continue;
            for (q = 0; q < s->max_voices; q++)
                if (q != a && s->v[q].on) { if (q > a) hi = 1; else lo = 1; }
            if (hi || !lo) ctl_oth[g] = (uint8_t)hi; else par = 0;
        }
#endif
        mx_newest[0] = newest[0]; mx_newest[1] = newest[1];
        ctl_n = nc; ctl_n0 = nc - nc / 2;
        two = par && nc >= 2 && (ctl_par = 1, cpu2_run(ctl2_job, 0u));
        if (!two) ctl_n0 = nc;
        for (j = 0; j < ctl_n0; j++) SY_FAR(ctl2_one)(ctl_idx[j], m);
        if (two && cpu2_wait() != 0) SY_FAR(ctl2_job)(0u);
        if (ctl_par) {                  /* each part's saturation table, as its last voice in the order built it */
            ctl_par = 0;
            for (k = 0; k < 2; k++) {
                sy_synth *s = P[k];
                int t = s->patch[SAT_TYPE];
                if (t >= 1 && t <= 4)
                    for (j = 0; j < nc; j++) {
                        float drv;
                        if (MX_PART(ctl_idx[j]) != k) continue;
                        drv = job_b[ctl_idx[j]].sat_w;
                        if (!(drv >= 0.f && drv <= 1.f)) continue;
                        if (s->sat_type_built != t || s->sat_drive_built != drv) {
                            SY_FAR(sy_sat_build)(s, t, drv);
                            s->sat_type_built = (uint8_t)t;
                            s->sat_drive_built = drv;
                        }
                    }
            }
        }
        for (j = 0; j < nc; j++) {
            sy_voice_t *v = &mm->sh->v[ctl_idx[j]];
            if (v->on || v->aenv.prev > 0.f) job_idx[nv++] = ctl_idx[j];
            else v->fresh = 0;
            na[MX_PART(ctl_idx[j])]++;
        }
        PROF(1);
        mx_jin[0] = in[0]; mx_jin[1] = in[1];
        job_n = nv; job_n0 = SY_BLKC(split_n0)(nv);
#ifdef SY_LPT
        { const int n0 = SY_BLKC(split_lpt)(nv); if (n0 >= 0) job_n0 = n0; }
#endif
#ifdef SY_FX_PIPE
        if (pipe && mx_skip && P[0]->fx_rest) {   /* (both parts' effects at rest, most likely: here, before the
                                         * voices, as the other core would have run them; nothing to hand out but
                                         * the voices) */
            fx2(out_l, out_r, pipe_l, pipe_r, pipe_bl, pipe_br, pipe_z);
            fx_done = 1;
            mx_fx = 0;
            two = job_n0 < nv && cpu2_run(voices2_job, 0u);
        } else if (pipe) {
            mx_ol = out_l; mx_or = out_r; mx_fx = 1;
            two = cpu2_run(voices2_job, 0u);
            fx_done = two;
            if (!two) mx_fx = 0;
        } else {
            mx_fx = 0;
            two = job_n0 < nv && cpu2_run(voices2_job, 0u);
        }
#else
        two = job_n0 < nv && cpu2_run(voices2_job, 0u);
#endif
        split_ran(two ? job_n0 : nv, nv);
        {
            LPT_T0();
            for (j = 0; j < (two ? job_n0 : nv); j++) {
                int g = job_idx[j], q2 = MX_PART(g);
                SY_FAR(sy_voice_render_w)(P[q2], &mm->sh->v[g], &job_b[g], vl[q2], vr[q2], in[q2], 0);
                LPT_V(g);
            }
        }
        PROF(2);
        if (two && cpu2_wait() != 0) SY_FAR(voices2_job)(0u);
        PROF(3);
        if (two)
            for (j = job_n0; j < nv; j++) {
                int q2 = MX_PART(job_idx[j]);
                for (q = 0; q < SY_BLOCK; q++) { vl[q2][q] += job_out[j - job_n0][0][q]; vr[q2][q] += job_out[j - job_n0][1][q]; }
            }
        for (j = 0; j < nv; j++) mm->sh->v[job_idx[j]].fresh = 0;
    } else
#endif
    for (k = 0; k < 2; k++) {
        sy_synth *s = P[k];
        if (k) {                        /* (part B's voices from a cleared block, as alone: a field a voice's path
                                         * leaves unset is its part's previous voice's, not A's) */
            volatile unsigned char *pz = (volatile unsigned char *)&b;
            unsigned q;
            for (q = 0; q < sizeof b; q++) pz[q] = 0;
        }
        for (i = 0; i < s->nv; i++) {
            sy_voice_t *v = &s->v[i];
            if (!v->on) continue;
            SY_FAR(voice_block)(s, v, &b, m, i == newest[k]);
            if (ARP_KEEP_PREV) {
                uint32_t *h = v->hph[s->block & 3];
                h[0] = v->ph1; h[1] = v->ph2; h[2] = v->ph3; h[3] = v->phs;
            }
            if (v->on || v->aenv.prev > 0.f) sy_voice_render(s, v, &b, vl[k], vr[k], in[k]);
            v->fresh = 0;
            na[k]++;
        }
    }
    for (k = 0; k < 2; k++) {
        sy_synth *s = P[k];
        if (inm[k] == 2) static_block(s, &b, m, vl[k], vr[k], in[k]);
        s->pv_jump = 0;
        s->part_snd = na[k] > 0 || inm[k] >= 1;
        part_mix(s, vl[k], vr[k], in[k], inm[k]);
    }
    PROF(4);
    {
        const int za = na[0] == 0 && inm[0] == 0;   /* (part A's sum holds zeros) */
#ifdef SY_FX_PIPE
        if (!pipe) fx2(out_l, out_r, vl[0], vr[0], vl[1], vr[1], za);
        else {
            if (!fx_done) fx2(out_l, out_r, pipe_l, pipe_r, pipe_bl, pipe_br, pipe_z);
            if (!(za && pipe_z)) for (i = 0; i < SY_BLOCK; i++) { pipe_l[i] = vl[0][i]; pipe_r[i] = vr[0][i]; }
            for (i = 0; i < SY_BLOCK; i++) { pipe_bl[i] = vl[1][i]; pipe_br[i] = vr[1][i]; }
            pipe_z = (uint8_t)za;
        }
#else
        fx2(out_l, out_r, vl[0], vr[0], vl[1], vr[1], za);
#endif
    }
    out_zero = fx2_zero;
    PROF(5);
    if (clip_in_render)
        for (i = 0; i < SY_BLOCK; i++) {
            float l = out_l[i], r = out_r[i];
            if (P[0]->opt & SY_OPT_SOFTCLIP) { l = sy_tanh(l); r = sy_tanh(r); }
            out_l[i] = sy_clampf(l, -1.f, 1.f);
            out_r[i] = sy_clampf(r, -1.f, 1.f);
        }
    part_post2(P[0]);
    part_post2(P[1]);
    PROF(6);
}

void sy_multi_render(sy_multi_t *m, float *out_l, float *out_r, const float *in_l, const float *in_r)
{
    mx_m = m;
    multi_setup(m);
    if (m->mode_now == SY_M_SINGLE) sy_render(m->p[0], out_l, out_r, in_l, in_r);
    else render2(m, out_l, out_r, in_l, in_r);
}

int sy_multi_render_i24(sy_multi_t *m, int32_t *lr, float g)
{
    float *restrict l = sy_work.l, *restrict r = sy_work.r;
    int i;
    clip_in_render = 0;
    sy_multi_render(m, l, r, 0, 0);
    clip_in_render = 1;
    if (out_zero) {                     /* (the effects at rest: zeros, known) */
        for (i = 0; i < 2 * SY_BLOCK; i++) lr[i] = 0;
        return 1;
    }
    SY_FAR(i24_out)(lr, l, r, g, (m->p[0]->opt & SY_OPT_SOFTCLIP) != 0);
    return 0;
}
#endif
