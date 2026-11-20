/* SPDX-License-Identifier: GPL-3.0-only */
/* The cleaner's own reverb (modes 2-4; PROVENANCE.md "reverb").
 *
 * Design (this project's): the mono input (the sum of both sides x the send's input gain) through the colour (a
 * one-pole low or high pass), a pre-delay line that is also the feedback echo of modes 3 and 4, four allpass
 * diffusers, then a tank of two halves in a figure eight: each half an allpass and a delay line that take the diffused
 * input and the other half's end (the allpass smears what circulates on every pass: without it the tail's envelope
 * repeated with the loop, 46-87 ms), its end read through the LFO's delay (Lagrange interpolation), then a damping
 * low pass and the decay gain. The feedback echo is written back inverted (as the development engine's: a repeat against the one
 * before). The right output taps the first half's line near its start and once more through the LFO's
 * delay further on; the left the second half's the same. A triangle LFO (the second half's half a cycle on) moves
 * the reads. REVERB HALF runs the same network at half the rate (half the lengths) between a decimating and an
 * interpolating half-band FIR, on half the ring (64 KB float, or 16-bit words in the two-part Bluetooth build, which
 * also runs FULL on its 16-bit ring of 64 KB while Bluetooth's memory is not lent). The rooms do not clear the ring:
 * every line is as long as the room needing most needs, the pre-delay the rest (a room change goes on sounding).
 * Below ~220 Hz the wet comes from a resonator bank fitted to the reference's measured LF response (RV_LF, section 17
 * of PROVENANCE.md); the network's input is high-passed there.
 *
 * Every number below is either a design choice (marked DESIGN) or measured on the development engine's reverb as a black box
 * (MEASURED, with the script in cleaner/tools/measure/reverb/ that measures or fits it): no source, table or memory of
 * the reference or of a reverb built elsewhere was read. */
#include "sy_int.h"
#include "sy_cln_rv.h"
#include "sy_tables.h"

#if defined(SY_FX_RAM) || defined(SY_RV_RAM)
#define RVR __attribute__((section(".ram_text")))   /* (the per-sample loops in RAM: a cold flash line costs ~500
                                         * cycles on the FM-1) */
#else
#define RVR
#endif
#define RVCOLD static __attribute__((noinline)) SY_COLD

/* RV_HALF_F (1.18C41): in the 16-bit builds REVERB HALF runs on a float ring, with
 * Bluetooth on or off: HALF's 16384 words as floats are the 16-bit ring's 64 KB (as 16-bit words HALF used half of it),
 * so no lent memory is needed. The network is the 32-bit builds' HALF (rv_net_fh), the float ring's rest (RVF_GATE)
 * ends its tail; no 16-bit floor (-93 dBFS rms on a test patch), no 16-bit gate's step, and the float loop costs less
 * than the 16-bit one (no rounding, no dither). Its clear takes the 2 blocks of the 16-bit HALF's (rv_clear_step):
 * the network's positions and LFO as 1.18C40's, the output 1.18C40's less its 16-bit noise (-84 .. -97 dBFS rms on
 * the reverb's null cases). The shimmer (HALF, Bluetooth off) keeps its buffer in the lent memory below. 0: HALF on
 * 16-bit words as 1.18C40 */
#if HYB_RV_BITS == 16 && !defined(HYB_RV_F16)
#ifndef RV_HALF_F
#define RV_HALF_F 1
#endif
#else
#undef RV_HALF_F
#define RV_HALF_F 0
#endif

/* RVQ_FADE (1.18C41): the gate does not clear the ring at once (1.18C40: the output went
 * from its floor, -92 dBFS rms / -79 peak on a test patch, to digital silence in one block, a step heard at a high
 * monitoring gain). Once the gate's count is reached the wet part fades out over RVQ_FADE blocks, its level falling
 * by RVQ_FADE_DB in the shape of half a cosine in dB (no corner at either end), then the ring is cleared as before
 * (the gate's clear: the network silent for its blocks, the effects rest once it is done). The network runs on during
 * the fade. A sound at the input during the fade takes it back: its level back to the full within that block (a
 * straight ramp, before the new sound's own reverb reaches the output past the pre-delay and the taps). Decay 127 set
 * during the fade takes it back too. The count, the shape and the clear in the flash (rvq_fade); in RAM a test a block.
 * 0: the clear at once as 1.18C40 */
#if HYB_RV_BITS == 16 && !defined(HYB_RV_F16)
#ifndef RVQ_FADE
#define RVQ_FADE 360                    /* (blocks: 0.98 s) */
#endif
#else
#undef RVQ_FADE
#define RVQ_FADE 0
#endif
/* RVQ_PDZ (1.18C41): in modes 3 and 4 (the feedback echo through the pre-delay, gain
 * FBS x A 115 / 128) the 16-bit pre-delay's writes below RVQ_PDZ steps are truncated toward zero instead of rounded
 * with the dither. From a feedback of about 96 (|gain| > 0.75) the dithered rounding holds small words forever (1 ->
 * -0.78 +- 0.25 rounds to -1, and back): a limit cycle that the tank turns into a tail at -64 .. -67 dBFS peak that
 * never ends (1.18C40, mode 3, feedback 100 or 110: no rest after 90 s; the float ring rests at 15-20 s). Toward zero
 * below the dead band a loop with a gain below 1 ends at 0 (as the delay's DL_DEADBAND): feedback 110 rests at 9.9 s,
 * 100 with the longest pre-delay at 23 s; 4 steps were not enough there (no rest), 8 rested at 47 s. Above the band
 * as before, so the echoes keep their level (a step lost a pass only below 16 steps, -60 dBFS of a word). Mode 2 (no
 * feedback) as before. 0: as 1.18C40 */
#if HYB_RV_BITS == 16 && !defined(HYB_RV_F16)
#ifndef RVQ_PDZ
#define RVQ_PDZ 16.f                    /* (steps) */
#endif
#else
#undef RVQ_PDZ
#define RVQ_PDZ 0
#endif
#ifndef RVQ_FADE_DB
#define RVQ_FADE_DB 72.f                /* (the floor's peak, -79 dBFS on a test patch, -75 at decay 124, to -147 / -151:
                                         * below the 24-bit output's last bit, -138.5) */
#endif

/* RV_SLOT (CPU plan section 15, stage A; the two-part Bluetooth firmware, HYB_RV_BITS 16 with .rt_spare): of the
 * network's variants only the one in use is in RAM. rv_net_f (FULL on the lent float ring), rv_net_h (HALF) and
 * rv_net_hf (FULL on the 16-bit ring) are in the flash (.text.rv_var); rv_path copies the one its layout needs into
 * rv_slot (RV_SLOT_BYTES in .rt_spare, NOLOAD) when the layout changes, on the block that starts the ring's clear (the
 * network silent: it runs again only once the clear is done), on the core that runs the network: no other code runs
 * it meanwhile. The variants are position-independent (their own branches relative, every call through a register:
 * rv_mix1 / rv_mix2 by SY_FAR; rv_slot_check.py checks it on the build, and their size against the slot). The same
 * code runs, from another address: the sound is the same. 0: the three in .ram_text (as 1.18C35) */
#if !defined(RV_SLOT) && HYB_RV_BITS == 16 && defined(HYB_RT_SPARE) && !defined(PLAT_HOST) && (defined(SY_FX_RAM) || defined(SY_RV_RAM))
#define RV_SLOT 1
#endif
#ifndef RV_SLOT
#define RV_SLOT 0
#endif
#if RV_SLOT
#define RVV __attribute__((section(".text.rv_var"), aligned(4)))   /* (a variant: flash, copied into rv_slot) */
#define RV_MIXC(fn) SY_FAR(fn)          /* (a call from a variant: through a register) */
#ifndef RV_SLOT_BYTES
#if RV_HALF_F
#define RV_SLOT_BYTES 3184              /* (1.18C41: >= the largest variant, rv_net_hf 3134 B (3182 in 1.18C40); HALF's
                                         * float rv_net_fh 2500 B in place of rv_net_h's 3198: 80 B of the RAM code's
                                         * region back) */
#else
#define RV_SLOT_BYTES 3264              /* (>= the largest variant, rv_net_h 3226 B in 1.18C36: rv_slot_check.py) */
#endif
#endif
#else
#define RVV RVR
#define RV_MIXC(fn) fn
#endif

extern uint32_t hyb_rv_clk;             /* (sy_fx.c: the delay clock's pre-delay, frames x 256; 0 none) */

#ifndef RV_ROTON
#define RV_ROTON 1                      /* (DESIGN: the mixing's first stage turned, CP_ROT; 0 a plain Hadamard) */
#endif
#ifndef RV_NWF
#define RV_NWF 4                        /* (DESIGN: the modulated reads' Lagrange points, FULL / HALF: their
                                         * top's loss is the absorption's, fit_rv.py damp; 6 / 8 a little flatter) */
#endif
#ifndef RV_NWH
#define RV_NWH 4
#endif
#ifndef RV_LST
#define RV_LST 4                        /* (DESIGN: the modulated reads' weights every RV_LST frames in FULL, RV_LSTH
                                         * in HALF: 8 until 1.18C30, the delay's steps spurs at multiples of the rate
                                         * / 8, -64 dB against a sine in FULL, -70 in HALF; 4 / 2: -71 / -84 dB, rvbal) */
#endif
#ifndef RV_LSTH
#define RV_LSTH 2
#endif
#ifndef RV_MOD2
#define RV_MOD2 0                       /* (DESIGN: 1, the second pair, lines 2 and 3, modulated too, CP_MOD2: the
                                         * turned mixing smears as well at a quarter of the cost, perc_rv.py) */
#endif
/* ---- the design constants (CLN_RV_TUNE: overridable by a measurement build, cln_rv_tune[]) ---- */
enum {
    CP_DG1, CP_DG2, CP_DG3, CP_DG4,     /* the diffusers' gains (MEASURED: rv_struct.py, an allpass fit of the first
                                         * 15 ms of the right output, the mean of rooms 0, 2, 3) */
    CP_GM0, CP_GM1, CP_GM2, CP_GM3,     /* the late output's gain by room: the lines' ends summed with signs (FITTED:
                                         * fit_rv.py taps, the early envelope) */
    CP_LFO_HZ, CP_MOD_PP,               /* the modulation: one triangle LFO, its rate (Hz) (MEASURED: modkind_rv.py, the
                                         * development engine's responses repeat every 0.836 s, mirrored about its turning points,
                                         * changing at a constant speed: one triangle, free-running from the start),
                                         * its depth on the modulated pair (frames peak to peak at 46,875 Hz) (FITTED:
                                         * perc_rv.py mod, the tail's IF deviation) */
    CP_DAMP0, CP_DAMP1, CP_DAMP2, CP_DAMP3,   /* FULL: the absorption's extra decay at 8 kHz over the mid band, dB/s,
                                         * by room (MEASURED: fit_rv.py damp; nearly the same at every decay) */
    CP_K0, CP_K1, CP_K2, CP_K3,         /* the decay's effective loop length by room, x the nominal (FITTED: fit_rv.py) */
    CP_GAIN,                            /* the wet output gain (FITTED: fit_rv.py, the octave levels) */
    CP_GLIDE,                           /* the pre-delay's glide time constant, s (FITTED: glide.py) */
    CP_COL_LP0, CP_COL_LP1, CP_COL_HP0, CP_COL_HP1,   /* the colour: log2 of the corner (Hz) at 0 / 63 (low pass),
                                         * 65 / 127 (high pass) (FITTED: fit_rv.py, the colour sweep) */
    CP_DAMPH0, CP_DAMPH1, CP_DAMPH2, CP_DAMPH3,   /* HALF: the same, dB/s at 8 kHz (FITTED: fit_rv.py half) */
    CP_LFOPH,                           /* the LFO's phase at the engine's start (cycles; MEASURED: modkind_rv.py, a
                                         * turning point 0.38 s after the start) */
    CP_LSC,                             /* the lines' lengths x (DESIGN: the modal density against the ring's room) */
    CP_T60,                             /* 4 x 18: by room the mid octaves' T60 (s) at A 116 = 0, 8, .., 120, 124, 126
                                         * (127 holds: no decay but the damping's) (FITTED: fit_rv.py, to the development engine's
                                         * T30 of the octaves 250 Hz - 2 kHz) */
    CP_GR = CP_T60 + 72,                /* 4: the wet gain by room (FITTED: fit_rv.py, the octave levels) */
    CP_TAP0 = CP_GR + 4,                /* 4: the first output taps' gain by room (FITTED: fit_rv.py, the early envelope) */
    CP_ER = CP_TAP0 + 4,                /* 4: the further early taps' gain by room (FITTED: fit_rv.py taps) */
    CP_MODR = CP_ER + 4,                /* 4: the modulation's depth by room, x CP_MOD_PP (FITTED: perc_rv.py mod) */
    CP_GH = CP_MODR + 4,                /* HALF: the wet gain x (FITTED: fit_rv.py) */
    CP_GL,                              /* 4 x 18: by room the wet level (dB) at the decay nodes of CP_T60 (FITTED:
                                         * fit_rv.py, the mid octaves' level by decay) */
    CP_FBS = CP_GL + 72,                /* the feedback's sign (MEASURED: dens_rv.py, a repeat against the one before) */
    CP_MOD2,                            /* the second modulated pair's depth x the first's (DESIGN: perc_rv.py mod) */
    CP_ROT, CP_ROTPH,                   /* the mixing's first stage turned: two rotations' angles pi/4 + ROT x the
                                         * same LFO (radians, x CP_MODR by room, from -1 to 1), the first a phase
                                         * ROTPH (cycles) after the delay's, the second a quarter cycle more (DESIGN:
                                         * perc_rv.py fine, the late spectrum's roughness: the modes smeared, the whole
                                         * periodic with the LFO as the development engine's) */
    CP_N
};
#define RVT0 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f
#ifdef CLN_RV_TUNE
float cln_rv_tune[256] = {
#else
static const float cln_rv_tune[CP_N] = {
#endif
/* FIT BEGIN (written by tools/measure/reverb/apply_fit.py from fit_rv.py's results) */
    0.659f, -0.663f, 0.66f, -0.623f,   /* DG1 DG2 DG3 DG4 */
    0.06f, 0.06f, 0.06f, 0.06f,   /* GM0 GM1 GM2 GM3 */
    1.196f, 48.f, 7.212238f, 4.577542f,   /* LFO_HZ MOD_PP DAMP0 DAMP1 */
    3.29292f, 2.480171f, 1.f, 1.f,   /* DAMP2 DAMP3 K0 K1 */
    1.f, 1.f, 0.6f, 0.094f,   /* K2 K3 GAIN GLIDE */
    6.88016f, 14.5441f, 4.616852f, 11.86554f,   /* COL_LP0 COL_LP1 COL_HP0 COL_HP1 */
    0.09339686f, 0.03456969f, 0.05000946f, 0.01460362f,   /* DAMPH0 DAMPH1 DAMPH2 DAMPH3 */
    0.543f, 1.f, 0.03443767f, 0.2064299f,   /* LFOPH LSC T0_0 T0_1 */
    0.3417387f, 0.4552168f, 0.6523344f, 0.8370491f,   /* T0_2 T0_3 T0_4 T0_5 */
    1.141294f, 1.335875f, 1.593562f, 1.891881f,   /* T0_6 T0_7 T0_8 T0_9 */
    2.326481f, 2.898598f, 3.866573f, 6.316898f,   /* T0_10 T0_11 T0_12 T0_13 */
    16.53693f, 82.14142f, 364.978f, 364.978f,   /* T0_14 T0_15 T0_16 T0_17 */
    0.02950555f, 0.2703025f, 0.3725105f, 0.4727872f,   /* T1_0 T1_1 T1_2 T1_3 */
    0.6544006f, 0.8602658f, 1.167974f, 1.35628f,   /* T1_4 T1_5 T1_6 T1_7 */
    1.588986f, 1.875795f, 2.283078f, 2.851738f,   /* T1_8 T1_9 T1_10 T1_11 */
    3.754292f, 5.273502f, 8.75875f, 22.12019f,   /* T1_12 T1_13 T1_14 T1_15 */
    95.00713f, 285.3329f, 0.03185278f, 0.3179102f,   /* T1_16 T1_17 T2_0 T2_1 */
    0.4252742f, 0.5164817f, 0.6697199f, 0.8503198f,   /* T2_2 T2_3 T2_4 T2_5 */
    1.150216f, 1.33832f, 1.572169f, 1.878928f,   /* T2_6 T2_7 T2_8 T2_9 */
    2.271507f, 2.821954f, 3.658434f, 5.086121f,   /* T2_10 T2_11 T2_12 T2_13 */
    8.308845f, 19.6376f, 60.43193f, 254.6699f,   /* T2_14 T2_15 T2_16 T2_17 */
    0.09828512f, 0.3762636f, 0.477181f, 0.5783776f,   /* T3_0 T3_1 T3_2 T3_3 */
    0.7017242f, 0.8769169f, 1.153894f, 1.35511f,   /* T3_4 T3_5 T3_6 T3_7 */
    1.588105f, 1.883381f, 2.259584f, 2.78454f,   /* T3_8 T3_9 T3_10 T3_11 */
    3.612298f, 5.035465f, 8.016712f, 18.22946f,   /* T3_12 T3_13 T3_14 T3_15 */
    47.75936f, 246.5657f, 0.3736787f, 0.3668083f,   /* T3_16 T3_17 GR0 GR1 */
    0.3500078f, 0.5038486f, 2.f, 2.f,   /* GR2 GR3 TAP0 TAP1 */
    2.f, 1.4f, 2.2f, 2.2f,   /* TAP2 TAP3 ER0 ER1 */
    2.2f, 1.5f, 0.6f, 0.85f,   /* ER2 ER3 MODR0 MODR1 */
    0.75f, 0.85f, 1.006938f, -1.657003f,   /* MODR2 MODR3 GH GL0_0 */
    -2.115754f, -1.155797f, -0.7935167f, -0.4780615f,   /* GL0_1 GL0_2 GL0_3 GL0_4 */
    -0.2812963f, -0.09849541f, -0.06616581f, -0.03019849f,   /* GL0_5 GL0_6 GL0_7 GL0_8 */
    0.03019849f, 0.07420846f, 0.1424722f, 0.1791095f,   /* GL0_9 GL0_10 GL0_11 GL0_12 */
    0.184179f, 0.1602408f, 0.2220879f, 3.994327f,   /* GL0_13 GL0_14 GL0_15 GL0_16 */
    3.994327f, -1.472861f, -2.005801f, -1.514843f,   /* GL0_17 GL1_0 GL1_1 GL1_2 */
    -1.069917f, -0.6571288f, -0.462464f, -0.2410652f,   /* GL1_3 GL1_4 GL1_5 GL1_6 */
    -0.1597347f, -0.04849724f, 0.04849724f, 0.1361085f,   /* GL1_7 GL1_8 GL1_9 GL1_10 */
    0.199004f, 0.2701119f, 0.3381956f, 0.3964688f,   /* GL1_11 GL1_12 GL1_13 GL1_14 */
    0.4940363f, 0.4731786f, 5.158106f, -1.062577f,   /* GL1_15 GL1_16 GL1_17 GL2_0 */
    -1.683392f, -1.643802f, -1.285317f, -0.859239f,   /* GL2_1 GL2_2 GL2_3 GL2_4 */
    -0.5345797f, -0.2729116f, -0.1669249f, -0.04220006f,   /* GL2_5 GL2_6 GL2_7 GL2_8 */
    0.04220006f, 0.1760266f, 0.2870764f, 0.4320198f,   /* GL2_9 GL2_10 GL2_11 GL2_12 */
    0.5620626f, 0.6679441f, 0.8309343f, 0.9099862f,   /* GL2_13 GL2_14 GL2_15 GL2_16 */
    6.212235f, -0.8922801f, -1.423565f, -1.538642f,   /* GL2_17 GL3_0 GL3_1 GL3_2 */
    -1.336891f, -0.9843228f, -0.6577048f, -0.3272652f,   /* GL3_3 GL3_4 GL3_5 GL3_6 */
    -0.2192541f, -0.07003739f, 0.07003739f, 0.2427933f,   /* GL3_7 GL3_8 GL3_9 GL3_10 */
    0.4058539f, 0.5430302f, 0.6472612f, 0.7643916f,   /* GL3_11 GL3_12 GL3_13 GL3_14 */
    0.8663705f, 0.9394701f, 0.9723789f, -1.f,   /* GL3_15 GL3_16 GL3_17 FBS */
    0.f, 1.f, 0.125f,   /* MOD2 ROT ROTPH */
/* FIT END */
};
#define CP(k) cln_rv_tune[CP_##k]

/* MEASURED (meas_rv.py "pre": the first arrival at the cap of A 114): the pre-delay's longest by room, frames at
 * 46,875 Hz (the ring's 32768 words less the room's network) */
static const uint16_t RV_CAP[4] = {23533u, 18787u, 14064u, 6991u};
/* MEASURED (rv_struct.py): the rooms' scales, the network's share of the ring against room 3's (from the pre-delay
 * caps; the diffusers' delays agree) */
static const float RV_S[4] = {0.3583f, 0.5424f, 0.7256f, 1.f};
/* MEASURED (rv_struct.py): the diffusers' delays in room 3, frames at 46,875 Hz */
static const int16_t RV_DIF[4] = {121, 165, 314, 442};
/* MEASURED (rv_struct.py: the first arrivals in mode 4): the first output taps (right, left), frames after the
 * diffusers; not scaled by the room as the rest (the left less the right is 110-113 frames in every room) */
static const int16_t RV_TAPR[4] = {36, 53, 72, 100}, RV_TAPL[4] = {146, 165, 184, 213};
/* HALF's latency against FULL's (MEASURED: the first arrivals in mode 4, HALF less FULL: 20, 19, 16, 16 frames), less
 * the half-band filters' 14 and the halved taps' rounding: half-rate frames more on the taps */
static const int8_t RV_HLAT[4] = {2, 1, 0, 0};   /* (6, 5, 4, 4 with the 15-tap half-band filters, 14 frames; rvbal: 23
                                         * taps, 22 frames, 4 half-rate frames less here) */
/* the pre-delay's first frame: mode 2 / 3 one frame more than mode 4 (MEASURED: meas_rv.py "pre") */

/* The tank (DESIGN, 1.18C6R): a feedback delay network of RV_N lines in room 3 (x the room's scale RV_S, x CP_LSC,
 * each rounded to a prime: no two lines share a factor, the modes spread). Their sum (22.7 k frames, 0.48 s) is about
 * the ring's room left by the pre-delay's cap in room 3 (the modal density the ring allows); the shortest against the
 * longest about 1 : 2 (the echoes' spacing even, no line's period dominating) */
#define RV_N 8
static const int16_t RV_FL[RV_N] = {1913, 2179, 2459, 2687, 2971, 3229, 3511, 3769};
/* the further early taps (DESIGN, frames in room 3 x the room's scale): right on lines 2 and 4, left on lines 3 and 5
 * (the early envelope's body after the first taps: fit_rv.py taps fits their gain). Their signs (DESIGN, 1.18C29):
 * right + first tap + both early taps, left - first tap + both early taps. With the lines' input signs (+ - + + - + -
 * -) the early sum of each side is no longer one coherent lump below ~200 Hz (1.18C28: the right's three taps all in
 * phase there, 2.5 dB more bass in the first 50 ms than the reference's renders, and left / right correlated where
 * those are anti-correlated); chosen among the 32 sign patterns by the bass's side / mid and level in time windows
 * 0-400 ms of impulse responses against the reference's (black-box renders; PROVENANCE.md 8c). No cost: signs */
static const int16_t RV_ETR[2] = {677, 1381}, RV_ETL[2] = {811, 1523};

#define RVF_RATE 46875.f                /* the rate the frame counts above are given at */
#define RV_NL (5 + RV_N)                /* the lines: pre-delay, 4 diffusers, the tank's RV_N */
enum { L_D1, L_D2, L_D3, L_D4, L_PD, L_F0 };   /* (the ring's order: the diffusers side by side, run through one pointer;
                                         * each line's writes take the oldest frames of the next) */

/* the decimating / interpolating half-band FIR of HALF (DESIGN, rvbal: 23 taps, remez of the 12-tap full-band
 * filter to 8 kHz, the centre 1/2; after both -0.4 dB at 9 kHz, -2.2 at 10, -6.6 at 11, -24 at 12.7, the images and
 * the alias 68 dB down or more (16.4 - 23 kHz: 4 kHz content's images at 19.4 kHz). Until 1.18C30 15 taps: -1.4 dB at
 * 9 kHz, -3.7 at 10, -7.8 at 11, the images 50 dB down (a 4 kHz line's at 19.4 kHz -50 dB: the wet's
 * 16 - 20 kHz bands 20 - 40 dB above the reference's on mid-heavy patches). Distances 11, 9, .., 1 from the centre */
static const float HB[6] = {-0.0017598f, 0.0065673f, -0.0175256f, 0.0399193f, -0.0900623f, 0.3126318f};
#define HBN 22                          /* (the decimator's history, frames; the interpolators' 11 half-rate frames) */

typedef struct {
    uint32_t w, mask;                   /* the write position (frames of the network's rate), the ring's mask */
    int16_t room, half, mode, key;      /* the layout made for (room, half); key -1: none yet */
    int32_t base[RV_NL];                /* each line's place in the ring */
    int32_t len[RV_NL], dd[4];          /* (dd: the diffusers' delays in the room) */
    int32_t fl[RV_N];                   /* the tank's lines' delays (frames of the network's rate; lines 0, 1 (2, 3):
                                         * the modulated pairs, their least delay) */
    int32_t tr, tl, etr[2], etl[2];     /* the output taps (frames of the network's rate) */
    float pre, cap;                     /* the pre-delay (glided; frames at 46,875 Hz), its cap (the network's rate) */
    float lpz, z[RV_N];                 /* the colour's state, the absorption's */
    float ph;                           /* the LFO's phase (cycles); phs: set (at the first layout)
                                         * and step a frame */
    uint32_t mr;                        /* (the modulation's generator) */
    float dz[HBN], uz[2][HBN / 2];      /* HALF: the decimator's input, the interpolators' */
    uint8_t want_half, full, clear;     /* the switch: HALF asked (0: FULL, the host's default), FULL running;
                                         * clear: the ring being cleared (its part + 1) */
    uint8_t quiet;                      /* (rv_path: the fading block of a change done) */
    uint8_t w16;                        /* the ring's words 16-bit */
    uint8_t phs;
    uint32_t qr;                        /* (16-bit words: the dither's generator) */
    uint16_t qlow;                      /* (16-bit words: blocks with the input silent, the output below RVQ_GATE) */
    uint8_t qdone;                      /* (16-bit words: cleared in this silence; 2 being cleared by the gate; the
                                         * float ring's gate the same, 2: cleared by it) */
    uint8_t qfade;                      /* (the float ring's gate: the block of its fade, 1 .. RVF_FADE; 0 none) */
    uint8_t qz;                         /* (the float ring's gate: cleared, and only exact zeros in since) */
#if RVQ_FADE
    uint16_t qf;                        /* (16-bit words, RVQ_FADE: the block of the gate's fade, 1 .. RVQ_FADE; 0 none) */
#endif
} rv_t;
static rv_t RV;                         /* (.bss: zero at the start, key 0: no layout yet) */
static uint32_t rv_qk[3];               /* (hyb_rv_run: what rv_qc was made for; 0: nothing, a layout change) */
static float *rv_ring;                  /* (the ring this block runs on) */
#ifdef HYB_RV_LEND
static float *rv_lent;                  /* REVERB FULL's float ring of 128 KB (Bluetooth's memory lent), 0 none */
#endif

#if HYB_RV_BITS == 16 && !defined(HYB_RV_F16)
/* the 16-bit word (DESIGN): fixed point, a signed integer, 1.0 = 2^14 (+-2: the network's words peak at 1.4 in the
 * listening cases, the feedback's case the most; a held decay 127 at A 113 127 can reach 2.6 and is clamped there; 2^13
 * cost 6 dB of floor, 2^15 clipped the feedback's case: tools/measure/reverb/rv_q16.py); the loop runs in its units (the
 * input x 2^14 in, the outputs x 2^-14 out: a word read is a conversion, no scaling; the products and sums in float,
 * the accumulators' 24-bit mantissa above the words' 16 bits). Written clamped to +-32767 (no overflow, no wrap) and
 * rounded to the nearest: x + 1.5 x 2^23 holds the rounded integer in its low 16 bits (an add, no conversion;
 * truncation toward zero, the conversion's own rounding, takes up to a step a write: a loss that killed a held tail,
 * decay 127, at -48 dB within a second). Rounded, a loop with a gain near 1 holds a few steps forever (a limit cycle
 * that never lets the effects rest): a uniform dither of half a step (one value a frame) keeps the rounding unbiased,
 * and with the input below half a step and the output's peak below the gate (RVQ_GATE steps x sqrt(T60 / 0.4 s): the
 * rounding's noise circulates as long as the decay holds it) for RVQ_HOLD blocks the ring is
 * cleared (at decay 127, held, never): a tail ends near -90 dBFS (rv_q16.py). The interpolations' weights (Lagrange,
 * the pre-delay's) stay float: the modulated reads as fine as the float ring's. HYB_RV_F16: the earlier 16-bit float
 * word instead (sign, 4-bit exponent, 11-bit mantissa: no floor, 1.6 x the cycles) */
#define RV_W16 int16_t
#ifndef RVQ_GATE
#define RVQ_GATE 3.f                    /* (steps, the output's peak: below it no sound, rv_q16.py) */
#endif
#define RVQ_HOLD 16                     /* (blocks, 44 ms) */
#ifndef RVQ_ONE
#define RVQ_ONE 16384.f
#endif
#ifndef RVQ_DITH
#define RVQ_DITH 0.5f                   /* (steps peak to peak: a uniform dither added before the rounding, one value a
                                         * frame for every write; with the gate a decaying tail ends in silence, no
                                         * limit cycle, rv_q16.py) */
#endif
static inline __attribute__((always_inline)) int16_t q16(float x, float dq)
{
    sy_fu u;
    u.f = sy_minf(sy_maxf(x + dq, -32767.f), 32767.f) + 12582912.f;
    return (int16_t)u.u;
}
#define RD16(p, o) ((float)(p)[(o) & m])
#define RP16(p) ((float)*(p))
#define WR16(p, o, v) ((p)[(o) & m] = q16(v, dq))
#elif HYB_RV_BITS == 16
/* HYB_RV_F16: the 16-bit word (DESIGN): sign, 4-bit exponent, 11-bit mantissa, rounded: |x| from 2^-13 to 3.999 (above: the
 * largest), 2^-12 relative; below 2^-13 linear in steps of 2^-24 (a tail decays on, no hard floor). Branchless both
 * ways (selects) */
static inline __attribute__((always_inline)) uint16_t f2h(float x)
{
    sy_fu u, v;
    uint32_t a, c, l;
    u.f = x;
    v.u = u.u & 0x7fffffffu;
    l = (uint32_t)(int32_t)(v.f * 16777216.f + 0.5f);   /* (the linear code below 2^-13: 0 .. 2048) */
    a = v.u + 0x800u;                   /* (rounded to 11 bits) */
    a = a > 0x407ff800u ? 0x407ff800u : a;
    c = (a - 0x38800000u) >> 12;        /* (exponent 114 .. 128: codes 0x800 .. 0x7fff) */
    c = v.u < 0x39000000u ? l : c;
    return (uint16_t)(((u.u >> 16) & 0x8000u) | c);
}
static inline __attribute__((always_inline)) float h2f(uint16_t h)
{
    sy_fu u;
    const uint32_t c = h & 0x7fffu;
    const float lin = (float)(int32_t)c * (1.f / 16777216.f);
    u.u = (c << 12) + 0x38800000u;
    u.f = c < 0x800u ? lin : u.f;
    u.u |= (uint32_t)(h & 0x8000u) << 16;
    return u.f;
}
#endif

#if RVF_GATE
/* (1.18C39) the float ring's rest (DESIGN). A float ring has no floor: a tail decays at its T60
 * until the words are subnormal (2^-126, ~-760 dB) before the effects may rest (sy_fx.c SY_FX_REST): a test patch's one
 * short note ran the network ~100 s, a decay of 126 ~45 minutes. Instead: once the reverb's input is below RVF_IN
 * and the part's output (after the phaser, the EQ and the boost: every gain after the reverb but the master volume)
 * below RVF_OUT for RVF_HOLD blocks plus the pre-delay's, the wet part fades to 0 over RVF_FADE blocks, then the ring
 * and the states are set to 0 in one block (the network runs on: its positions, LFO and glide move as before, so a
 * later note finds them where it found them) and the effects rest as after any silence. What is dropped is the tail
 * below RVF_OUT: 18 dB under the 24-bit output's last bit (the device truncates to 24 bits), 28 dB under the user's
 * -92 dBFS. A sound during the hold or the fade: the count starts again, the fade is taken back (a step below
 * RVF_OUT). The 16-bit ring keeps its own gate at its floor (RVQ_GATE). Not at decay 127 (held). RVF_GATE 0: as
 * 1.18C38 */
#ifndef RVF_OUT
#define RVF_OUT 1e-6f                   /* (-120 dBFS: the part's output peak a block) */
#endif
#ifndef RVF_IN
#define RVF_IN 5.9604645e-8f            /* (2^-24, -144 dBFS: the reverb's input peak a block) */
#endif
#ifndef RVF_HOLD
#define RVF_HOLD 32                     /* (blocks, 87 ms: more than the network's longest loop, 57 ms) */
#endif
#ifndef RVF_FADE
#define RVF_FADE 16                     /* (blocks, 44 ms: a straight fade of the wet part) */
#endif
uint8_t hyb_rv_arm;
float hyb_rv_opk;
static void rv_states_clear(void);
/* a block with the input below RVF_IN (the float ring): the count, the fade's blocks, the ring set to 0 once faded out
 * (the flash: while the input sounds hyb_rv_run does not call it) */
static __attribute__((noinline)) SY_COLD void rvf_gate(const uint8_t *p, float a)
{
    if (RV.qdone == 1 || p[116] >= 127
#ifdef HYB_SHIMMER
        || hyb_rv_shim_on()
#endif
        ) { RV.qlow = 0; RV.qfade = 0; hyb_rv_arm = 0; return; }   /* (a fresh ring, decay 127 held, the shimmer) */
    if (RV.qdone) {                     /* (cleared in this silence) */
        if (a != 0.f) RV.qz = 0;
        return;
    }
    if (RV.qfade > RVF_FADE) {          /* (faded out: the ring and the states 0; the network runs on them) */
        uint32_t *w = (uint32_t *)(void *)rv_ring, n = RV.mask + 1u, i;   /* (the float words: rv_words) */
        for (i = 0; i < n; i++) w[i] = 0u;
        rv_states_clear();
        RV.qdone = 2; RV.qlow = 0; RV.qfade = 0; hyb_rv_arm = 0; RV.qz = a == 0.f;
        return;
    }
    if (RV.qlow && !(hyb_rv_opk < RVF_OUT)) { RV.qlow = 1; RV.qfade = 0; }   /* (the last block's output above: the
                                         * count from here; a fade taken back) */
    else if (RV.qfade) RV.qfade++;
    else if (++RV.qlow >= RVF_HOLD + (int)(RV.pre * (1.f / (float)SY_BLOCK)) + 1) RV.qfade = 1;
    hyb_rv_arm = 1;
}
void hyb_rv_post(const float *l, const float *r)   /* (sy_fx.c: the part's output peak, the flash) */
{
    float m = 0.f;
    int i;
    for (i = 0; i < SY_BLOCK; i++) m = sy_maxf(m, sy_maxf(sy_absf(l[i]), sy_absf(r[i])));
    hyb_rv_opk = m;
    hyb_rv_arm = 0;
}
#endif

/* ---- the layout ---- */
static int rv_room(const uint8_t *p) { return p[117] > 3 ? 3 : p[117]; }

static int rv_prime(int32_t n)          /* the least prime >= n (n >= 2) */
{
    int32_t d;
    if (n < 3) return 3;
    if (!(n & 1)) n++;
    for (;; n += 2) {
        for (d = 3; d * d <= n; d += 2) if (n % d == 0) break;
        if (d * d > n) return (int)n;
    }
}

static int rv_words(void);
static int32_t rv_depth(int room, int half)   /* the modulation's depth, whole frames of the network's rate */
{
    return (int32_t)(CP(MOD_PP) * (half ? 0.5f : 1.f) * (&CP(MODR))[room] + 0.5f);
}

RVCOLD void rv_rooms(int room)          /* the room's layout: the lines in the ring (the room's network, the pre-delay
                                         * the rest; a room change goes on sounding, its tail now read through the new
                                         * layout's lines, as the development engine's does: no clearing) */
{
    const float s = RV_S[room], k = RV.half ? 0.5f : 1.f;
    const int NW = RV.half ? RV_NWH : RV_NWF;
    const int32_t dep = rv_depth(room, RV.half);
    int32_t b = 0, used = 0, t;
    int i;
    RV.cap = (float)RV_CAP[room] * k;
    for (i = 0; i < 4; i++) {
        RV.dd[i] = (int32_t)((float)RV_DIF[i] * s * k + 0.5f);
        RV.len[L_D1 + i] = RV.dd[i] + 1;
    }
    RV.tr = (int32_t)((float)RV_TAPR[room] * k + 0.5f) + (RV.half ? RV_HLAT[room] : 0);
    RV.tl = (int32_t)((float)RV_TAPL[room] * k + 0.5f) + (RV.half ? RV_HLAT[room] : 0);
    for (i = 0; i < 2; i++) {
        RV.etr[i] = (int32_t)((float)RV_ETR[i] * s * k + 0.5f);
        RV.etl[i] = (int32_t)((float)RV_ETL[i] * s * k + 0.5f);
    }
    for (i = 0; i < RV_N; i++) {
        RV.fl[i] = rv_prime((int32_t)((float)RV_FL[i] * CP(LSC) * s * k + 0.5f));
        t = RV.fl[i] + (i < 4 ? dep + NW : 0) + 2 + (i ? 0 : SY_BLOCK);   /* (the modulated pairs: their depth, the
                                         * interpolation's points; the first: a block more, rv_net's pass 0) */
        if (t < RV.tr + 2) t = RV.tr + 2;
        if (t < RV.tl + 2) t = RV.tl + 2;
        RV.len[L_F0 + i] = t;
    }
    for (i = 0; i < RV_NL; i++) if (i != L_PD) used += RV.len[i] + 1;
    /* the pre-delay: the rest (its cap is within it: the lines' sum is chosen to leave it) */
    RV.len[L_PD] = rv_words() - 1 - used;
    for (i = 0; i < RV_NL; i++) { b += RV.len[i] + 1; RV.base[i] = b;   /* (line i: [w + base - len, w + base]) */ }
    RV.room = (int16_t)room;
    rv_qk[1] = 0u;                      /* (the parameters made again: the lines' gains follow their lengths) */
}

RVCOLD void rv_layout(int half)
{
    RV.half = (int16_t)half;
    rv_rooms(RV.room);
}

/* ---- the per-block parameters ---- */
typedef struct {
    float gin, dry, wet;                /* the send's */
    float b[RV_N], p[RV_N];             /* the absorption per line: its input's weight (the decay gain x (1 - p) x the
                                         * mixing's 1 / sqrt(RV_N)), its pole */
    float col_a;                        /* the colour's (0 off) */
    float col_kz;                       /* (its zero: -1 x kz; 1 the bilinear's, at the Nyquist rate) */
    int col_hp;
    float gfb;                          /* the feedback (modes 3, 4) */
    float pre0, pre1;                   /* the pre-delay over the block (frames of the network's rate) */
    int32_t lfo_l;                      /* the modulation's depth, whole frames (the pairs' mirrored weights) */
    int mode4;
    float t60;                          /* the mid T60 (s), -1 holding (127) */
    float gl;                           /* the wet level by decay (CP_GL) */
    float dph;                          /* the LFO's phase step a frame (cycles) */
    float gate;                         /* (16-bit words: the gate's level, steps of the output's peak) */
} rv_par;
static rv_par rv_qc;                    /* (hyb_rv_run: the last block's, rv_qk) */
#ifndef RV_LF
#define RV_LF 1                         /* the low-frequency path (rvbal, section 17); 0: the network alone */
#endif
#ifndef RVLF_FAST
#define RVLF_FAST 1                     /* (CPU plan P5: the LF path's arithmetic restructured, the same response: the
                                         * decimator's CIC in four stages of (1 + 1/z)^3 / 2 (no input copy), the
                                         * biquads inline, the bank both LF samples in one pass over its modes, the
                                         * x 16 straight lines added in hyb_rv_run's output loop; 0: the code of
                                         * section 17, the null between the two below -100 dBFS) */
#endif
#ifndef RVLF_SPLINE
#define RVLF_SPLINE RVLF_FAST           /* (DESIGN, after 1.18C37) the LF path's output from its x 4 rate (fs / 16) to fs:
                                         * a cubic B-spline through its points instead of straight lines. The straight
                                         * lines left images of the LF tail at k fs / 16 +- f (a G3's: 2.56 / 2.95, 5.3 /
                                         * 5.7, 8.1 / 8.5 kHz at 44.1 kHz), -45 dB re the LF wet: a high, inharmonic
                                         * ringing in a dull, soft note's reverb tail; the
                                         * spline puts them at about -90 dB (sinc^4 against sinc^2), for 16 frames more
                                         * of the LF path's delay (its pre-delay is in steps of 64 frames anyway) and
                                         * -0.3 dB at 200 Hz. 0: the straight lines (RVLF_FAST only) */
#endif
#if RV_LF
static void rvlf_clear(void);
static void rvlf_par(const rv_par *q, const uint8_t *p);
static void rvlf_hp(float *x, int n, int half);
#if RVLF_FAST
static void rvlf_run(const float *x, float pre);
#else
static void rvlf_in(const float *x);
static void rvlf_block(float *yl, float *yr, float pre);
#endif
static int rvlf_still(void);
#endif

static float tpt_g(float hz, float fs)  /* the bilinear one-pole's g / (1 + g), g = tan(pi f / fs) */
{
    float w = SY_PI * sy_minf(hz, 0.49f * fs) / fs, t = w + w * w * w / 3.f + 2.f * w * w * w * w * w / 15.f;
    if (w > 0.8f) t = FSIN2PI(w / SY_2PI) / FSIN2PI(0.25f - w / SY_2PI);
    return t / (1.f + t);
}

RVCOLD void rv_par_make(rv_par *q, const uint8_t *p, int mode)
{
    const float fs = RVF_RATE * (RV.half ? 0.5f : 1.f);   /* (DESIGN: the network counts frames at any rate:
                                         * the FM-1's 44.1 kHz too) */
    const int room = RV.room;
    const float k = RV.half ? 0.5f : 1.f, d = (float)p[116];
    float t60, x, c, P;
    int j;
    q->lfo_l = rv_depth(room, RV.half);
    q->t60 = -1.f;
    /* the decay: the T60 table's log interpolated; 127 holds */
    q->gl = 1.f;
    if (p[116] >= 127) {
        t60 = -1.f;
        q->gl = FDB2LIN((&CP(GL))[18 * room + 17]);
    } else {
        x = d < 120.f ? d / 8.f : d < 124.f ? 15.f + (d - 120.f) / 4.f : 16.f + (d - 124.f) / 2.f;
        j = (int)x;
        if (j > 16) j = 16;
        {
            const float *T = &CP(T60) + 18 * room;
            const float *G = &CP(GL) + 18 * room;
            t60 = FEXP2(FLOG2(T[j]) + (x - (float)j) * (FLOG2(T[j + 1]) - FLOG2(T[j])));
            q->gl = FDB2LIN(G[j] + (x - (float)j) * (G[j + 1] - G[j]));
        }
        q->t60 = t60;
    }
    {   /* per line (DESIGN, Jot's absorbent delays): its decay gain 10^(-3 L / (fs T60)) and a one-pole low pass whose
         * loss at 8 kHz is the room's extra decay (dB/s) x L / fs: every line loses the same per second at every
         * frequency (no line, no mode, rings longer than the rest). The pole p from the loss l (dB) at w:
         * (1 - 2 p cos w + p^2) / (1 - p)^2 = 10^(l / 10), the root below 1 */
        const float ke = (&CP(K0))[room];
        const float al = RV.half ? (&CP(DAMPH0))[room] : (&CP(DAMP0))[room];
        const float cw = FCOS2PI(8000.f / fs);
        for (j = 0; j < RV_N; j++) {
            const float L = ((float)RV.fl[j] + (j < (RV_MOD2 ? 4 : 2) ? 0.5f * (float)q->lfo_l : 0.f)) * ke;
            const float g = t60 < 0.f ? 1.f : FEXP2(-9.965784f * L / (fs * t60));   /* (2^-(log2 1000) L / (fs T60)) */
            const float qq = FEXP2(0.3321928f * sy_maxf(al, 0.f) * L / fs);    /* (10^(l / 10)) */
            float pp = 0.f;
            if (qq > 1.000001f) {
                const float B = (qq - cw) / (qq - 1.f);
                pp = 1.f / (B + SY_FAR(sy_sqrt)(B * B - 1.f));
            }
            q->p[j] = pp;
            q->b[j] = g * (1.f - pp) * 0.35355339f;   /* (x 1 / sqrt 8: the mixing orthonormal) */
        }
    }
    /* the colour (A 119): 64 flat, below a one-pole low pass, above a one-pole high pass, the corner exponential */
    c = (float)p[119];
    q->col_a = 0.f; q->col_hp = 0; q->col_kz = 1.f;
    if (c < 63.5f) {
        const float fc = FEXP2(CP(COL_LP0) + c / 63.f * (CP(COL_LP1) - CP(COL_LP0)));
        q->col_a = tpt_g(fc, fs);
        if (RV.half) {                  /* (DESIGN, rvbal: HALF's bilinear low pass has its zero at its Nyquist rate,
                                         * 11.7 kHz: 2 - 7 dB more loss at 8 - 10 kHz than FULL's for corners of 2 - 5
                                         * kHz (colour 34 - 45). Instead the pole impulse-invariant, p = exp(-2 pi fc /
                                         * fs), and the zero -kz chosen so the gain at 11.7 kHz is FULL's there:
                                         * H = a (1 + kz / z) / (1 - p / z), a = (1 - p) / (1 + kz), DC 1; the loop's
                                         * state update lpz = y + kz c) */
            const float p = FEXP2(-9.0647202f * fc / fs);   /* (2 pi / ln 2) */
            const float t = tpt_g(fc, RVF_RATE), g = t / (1.f - t);   /* (FULL's tan(pi fc / fs)) */
            const float gn = g * SY_FAR(sy_sqrt)(1.f / (1.f + g * g));   /* (FULL's gain at 11.7 kHz: tan = 1 there) */
            const float r = gn * (1.f + p) / (1.f - p);
            const float kz = sy_maxf(0.f, sy_minf(1.f, (1.f - r) / (1.f + r)));
            q->col_kz = kz;
            q->col_a = (1.f - p) / (1.f + kz);
        }
    }
    else if (c > 64.5f) { q->col_a = tpt_g(FEXP2(CP(COL_HP0) + (c - 65.f) / 62.f * (CP(COL_HP1) - CP(COL_HP0))), fs); q->col_hp = 1; }
    /* the pre-delay (frames at 46,875 Hz): the delay clock's note value, halved until it fits; else 256 x A 114 up
     * to the room's cap; one frame more in modes 2 and 3 (MEASURED) */
    if (hyb_rv_clk) {
        uint32_t w = hyb_rv_clk >> 8;
        while ((float)w + 1.f > (float)RV_CAP[room]) w >>= 1;
        P = (float)w + 1.f;
    } else P = sy_minf(256.f * (float)p[114] + 1.f, (float)RV_CAP[room]);
    if (P * k > (float)(RV.len[L_PD] - 2)) P = (float)(RV.len[L_PD] - 2) / k;   /* (the line's end) */
    q->pre1 = (float)(int32_t)(P * k + 0.5f);   /* (the target: hyb_rv_run glides to it; whole frames, HALF's
                                         * too: no interpolation's loss on the feedback's repeats) */
    if (q->pre1 < 1.f) q->pre1 = 1.f;
    q->mode4 = mode == 4;
    q->gfb = mode >= 3 ? CP(FBS) * (float)p[115] * (1.f / 128.f) : 0.f;
    q->dph = CP(LFO_HZ) / fs;
    /* (16-bit words: the rounding's noise circulates as long as the decay holds it, its floor ~ sqrt(T60): the gate's
     * level RVQ_GATE x sqrt(T60 / 0.4 s), at least RVQ_GATE, at most 8 x: -84 dBFS at decay 104 cleared) */
#ifdef RVQ_GATE
    q->gate = RVQ_GATE * sy_minf(8.f, sy_maxf(1.f, SY_FAR(sy_sqrt)(sy_maxf(t60, 0.f) * 2.5f)));
#else
    q->gate = 0.f;
#endif
#if RV_LF
    rvlf_par(q, p);
#endif
}

/* ---- the modulation (DESIGN, the development engine's measured kind: modkind_rv.py): one triangle LFO, free-running from the
 * engine's start (on through silence: hyb_rv_quiet), the whole reverb periodic with it. The modulated pair's delays
 * follow it (the second line's delay the depth less the first's: the same weights mirrored), the mixing's two turned
 * rotations follow it at other phases. Made a block at a time: the delay at the block's end, a straight line through
 * the block (the triangle is one but at its two turning points) ---- */
SY_BLKR static inline float rv_tri(float x)   /* 0 .. 1 .. 0 over a cycle */
{
    x -= (float)(int32_t)x;
    if (x < 0.f) x += 1.f;
    return x < 0.5f ? 2.f * x : 2.f - 2.f * x;
}

RVCOLD SY_BLKR void rv_mod_block(const rv_par *q, int n, float *m0, float *dm, float *cs)
{
    const float L = (float)q->lfo_l - 0.002f;
    const float ra = (&CP(MODR))[RV.room] * CP(ROT) * (1.f / SY_2PI);
    const float p0 = RV.ph, p1 = p0 + q->dph * (float)n;
    int j;
    for (j = 0; j < 2; j++) {           /* (the pairs' delays: 0 .. the depth; the second pair a quarter on) */
        const float sc = L * (j ? CP(MOD2) : 1.f), o = 0.25f * (float)j;
        m0[j] = sc * rv_tri(p0 + o);
        dm[j] = (sc * rv_tri(p1 + o) - m0[j]) / (float)n;
    }
    for (j = 0; j < 2; j++) {           /* (the rotations: sqrt 2 (cos, sin) of pi / 4 + ROT x the LFO in -1 .. 1, at
                                         * the block's start and a step a frame to its end: 1.18C30 held them a block,
                                         * a gain step every 128 frames that spread spurs over the whole band, -49 dB
                                         * against a sine, rvbal) */
        const float a = 0.125f + ra * (2.f * rv_tri(p0 + CP(ROTPH) + 0.25f * (float)j) - 1.f);
        const float b = 0.125f + ra * (2.f * rv_tri(p1 + CP(ROTPH) + 0.25f * (float)j) - 1.f);
        cs[2 * j] = 1.41421356f * sy_cos2pi(a); cs[2 * j + 1] = 1.41421356f * sy_sin2pi(a);
        cs[4 + 2 * j] = (1.41421356f * sy_cos2pi(b) - cs[2 * j]) / (float)n;
        cs[5 + 2 * j] = (1.41421356f * sy_sin2pi(b) - cs[2 * j + 1]) / (float)n;
    }
    RV.ph = p1 - (float)(int32_t)p1;
}

/* ---- the network, one sample of its rate: x in, the right and left outputs. W: the ring's word type ---- */
#if HYB_RV_BITS == 16 && defined(HYB_RV_F16)
#define RV_W16 uint16_t
#define RVQ_ONE 1.f
#define RD16(p, o) h2f((p)[(o) & m])
#define RP16(p) h2f(*(p))
#define WR16(p, o, v) ((p)[(o) & m] = f2h(v))
#endif
#define RDF(p, o) ((p)[(o) & m])
#define RPF(p) (*(p))
#define WRF(p, o, v) ((p)[(o) & m] = (v))

/* the modulated lines' ends read between frames (DESIGN): Lagrange interpolation, RV_NWF points in FULL, RV_NWH in HALF
 * (two lines of eight: their mean loss at the top is taken into the absorption's fit, its swing with the fraction
 * small against the other six lines') */
#define LAGW(NW, f, wt)                                                                                       \
    {   float pre_[NW], suf_ = 1.f; int k_;                                                                   \
        pre_[0] = 1.f;                                                                                        \
        for (k_ = 1; k_ < NW; k_++) pre_[k_] = pre_[k_ - 1] * ((f) - (float)(k_ - 1 - (NW / 2 - 1)));         \
        for (k_ = NW - 1; k_ >= 0; k_--) { wt[k_] = pre_[k_] * suf_ * LAGD##NW[k_];                           \
                                           suf_ *= (f) - (float)(k_ - (NW / 2 - 1)); } }
static const float __attribute__((unused)) LAGD4[4] = {-1.f / 6.f, 1.f / 2.f, -1.f / 2.f, 1.f / 6.f};
static const float __attribute__((unused)) LAGD6[6] = {-1.f / 120.f, 1.f / 24.f, -1.f / 12.f, 1.f / 12.f, -1.f / 24.f, 1.f / 120.f};
static const float __attribute__((unused)) LAGD8[8] = {-1.f / 5040.f, 1.f / 720.f, -1.f / 240.f, 1.f / 144.f, -1.f / 144.f, 1.f / 240.f,
                               -1.f / 720.f, 1.f / 5040.f};   /* (1 / prod (k - j) over the nodes -2 .. 3, -3 .. 4) */
#ifndef RVQ_DITH
#define RVQ_DITH 0.f
#endif

/* ---- the network over a block (DESIGN): one ring, one write position, every line a fixed offset from it; the work
 * in passes that each touch few lines, so the ring is read and written in straight runs (a run that would cross the
 * ring's end, a few blocks in each pass of the ring, through the masked reads instead):
 *   0  the input's chain a frame at a time (the colour, the pre-delay line with the feedback written back, its read
 *      linear between frames while it glides, the four diffusers side by side through one pointer): the diffused
 *      input U
 *   A  the tank's lines' ends (each line is longer than a block: what a block reads was written before it): the
 *      modulated pair(s) through NW-point Lagrange (weights every RV_LST frames), into rv_o; the
 *      plain lines are read by B where they are in the ring (no copy)
 *   B  the late outputs (right lines 0 - 2 + 4 - 6, left 1 - 3 + 5 - 7: the sides from different lines,
 *      uncorrelated), the mixing (an 8 x 8 Hadamard matrix in three stages of sums and differences, two pairs of its
 *      first stage turned by slowly moving angles): three short loops (few values live each) over a stretch of the
 *      block in which no plain line's run crosses the ring's end
 *   C  each line written in a run through its absorption (a one-pole low pass x the decay gain, with the mixing's
 *      1 / sqrt 8; the same in the loop on this side of the delay as on the other), the diffused input added (+ - + +
 *      - + - -: the lines of one sign, then the input negated, the rest: no multiply); then the taps (a first tap and
 *      two early taps a side, on lines near their start; runs that cross the ring's end split in two)
 * (A - C and the half-band's copies reworked for the instruction count, the sound bit for bit the same:
 * PROVENANCE.md 8b)
 * The first tank line is a block longer than it needs (rv_rooms): the pre-delay's writes of pass 0 run ahead of its
 * reads. W: the ring's word type; SC its scale (16-bit: the words' units, x 2^14) */
#define RV_NB SY_BLOCK
SY_WORK_ATTR static float rv_u[RV_NB];
#ifndef RV_SPLIT
#define RV_SPLIT 1                      /* (CPU plan P5: the network's mixing (one function for every variant) and
                                         * the line writes (one for FULL and HALF) in small functions of their own:
                                         * their loops get the registers the big function's live values took (the
                                         * target's 16 hold integers and floats: spills, moves; the 16-bit line write
                                         * 19 -> 13 instructions a word). The same arithmetic in the same order: bit
                                         * for bit as before. 0: the loops inline as before. (Tried: the plain lines
                                         * read in place by the mixing, no copies: more pointers live, spills, no gain
                                         * on the target) */
#endif
#ifndef RV_ILV
#define RV_ILV 1                        /* (CPU plan P5, with RV_SPLIT: the lines' block buffer frame by frame, the 8
                                         * lines of a frame side by side: the mixing's loops address a frame's lines
                                         * at small fixed offsets from one pointer (on the target 27 + 34 instead of
                                         * 30 + 40 instructions a frame); the same numbers) */
#endif
#if RV_ILV && RV_SPLIT
SY_WORK_ATTR static float rv_oi[RV_NB][RV_N];
#define RVO(l) (&rv_oi[0][l])           /* (line l's first frame; its frames RVOS apart) */
#define RVOS RV_N
#else
SY_WORK_ATTR static float rv_o[RV_N][RV_NB];
#define RVO(l) (rv_o[l])
#define RVOS 1
#endif
#if HYB_RV_BITS == 16
SY_WORK_ATTR static float rv_dq[RV_NB];   /* (16-bit words: the dither of a block's writes) */
#else
#define rv_dq rv_u                      /* (unused) */
#endif
#ifndef RVQ_DITH
#define RVQ_DITH 0.f
#endif
#define RVQ_DQ(qr) (RVQ_DITH * (1.f / 4294967296.f) * (float)(int32_t)(qr))
/* the mixing over the block (RV_SPLIT; for every word type: the lines' floats): the late outputs into yr, yl, the first
 * two stages of the mixing into the lines' buffer; the first two loops of the big function's before, each a function (each loop
 * alone in the registers). The third stage (the sums and differences of lines l and l + 4) is taken by the line
 * writes (rv_cw_*: a load and an add a word instead of a pass over the 8 lines) */
RVR static __attribute__((noinline)) void rv_mix1(int n, float *yr, float *yl, const float *cs)
{   /* (the first stage and the second of lines 0 - 3) */
    float *const O0 = RVO(0), *const O1 = RVO(1), *const O2 = RVO(2), *const O3 = RVO(3);
    float cs0 = cs[0], cs1 = cs[1];
    const float dc0 = cs[4], dc1 = cs[5];
    int i;
    for (i = 0; i < n; i++) {
        const float z0 = O0[RVOS * i], z1 = O1[RVOS * i], z2 = O2[RVOS * i], z3 = O3[RVOS * i];
        float a2, a3;
        const float a0 = z0 + z1, a1 = z0 - z1;
        yr[i] = z0 - z2; yl[i] = z1 - z3;
        if (RV_ROTON) { a2 = cs0 * z2 + cs1 * z3; a3 = cs1 * z2 - cs0 * z3; cs0 += dc0; cs1 += dc1; }
        else { a2 = z2 + z3; a3 = z2 - z3; }
        O0[RVOS * i] = a0 + a2; O2[RVOS * i] = a0 - a2; O1[RVOS * i] = a1 + a3; O3[RVOS * i] = a1 - a3;
    }
}
RVR static __attribute__((noinline)) void rv_mix2(int n, float *yr, float *yl, const float *cs, float gm)
{   /* (the same of lines 4 - 7, with the outputs) */
    float *const O4 = RVO(4), *const O5 = RVO(5), *const O6 = RVO(6), *const O7 = RVO(7);
    float cs2 = cs[2], cs3 = cs[3];
    const float dc2 = cs[6], dc3 = cs[7];
    int i;
    for (i = 0; i < n; i++) {
        const float z4 = O4[RVOS * i], z5 = O5[RVOS * i], z6 = O6[RVOS * i], z7 = O7[RVOS * i];
        float a6, a7;
        const float a4 = z4 + z5, a5 = z4 - z5;
        yr[i] = gm * (yr[i] + (z4 - z6)); yl[i] = gm * (yl[i] + (z5 - z7));
        if (RV_ROTON) { a6 = cs2 * z6 + cs3 * z7; a7 = cs3 * z6 - cs2 * z7; cs2 += dc2; cs3 += dc3; }
        else { a6 = z6 + z7; a7 = z6 - z7; }
        O4[RVOS * i] = a4 + a6; O6[RVOS * i] = a4 - a6; O5[RVOS * i] = a5 + a7; O7[RVOS * i] = a5 - a7;
    }
}
/* a line's run written through its absorption (RV_SPLIT): p the ring's run, the mixed line the mixing's third stage
 * oa OP ob (OP + for lines 0 - 3: line l + line l + 4, - for 4 - 7: line l - 4 - line l), u the diffused input
 * (its sign for this line), dd the dither (16-bit words); the absorption's state in and out */
#define RV_CW_FN(name, T, WP, SC, OP)                                                                         \
RVR static __attribute__((noinline)) float name(T *p, const float *oa, const float *ob, const float *u,       \
                                                const float *dd, int n, float bb, float pp, float z)          \
{                                                                                                             \
    int i;                                                                                                    \
    float dq = 0.f;                                                                                           \
    (void)dq; (void)dd;                                                                                       \
    for (i = 0; i < n; i++) {                                                                                 \
        if ((SC) != 1.f) dq = dd[i];                                                                          \
        z = bb * (oa[RVOS * i] OP ob[RVOS * i]) + pp * z; WP(p + i, z + u[i]);                                  \
    }                                                                                                         \
    return z;                                                                                                 \
}

#define RV_NET_FN(name, T, RD, WR, RP, WP, NW, SC, LST, CWP, CWM)                                                 \
RVV static __attribute__((noinline)) void name(const rv_par *q, const float *xin, float *yl, float *yr, int n)  \
{                                                                                                             \
    T *const rg = (T *)(void *)rv_ring;                                                                       \
    const uint32_t m = RV.mask, M = RV.mask + 1u, w = RV.w;                                                   \
    float *const U = rv_u;                                                                                    \
    int i, l, ph;                                                                                                 \
    {   /* 0: the input's chain (the diffusers through one pointer and the pre-delay's write and read through their\
         * own while no run crosses the ring's end and the pre-delay holds still; else masked) */             \
        uint32_t qr = RV.qr, ww = w;                                                                          \
        float lpz = RV.lpz, pre = q->pre0;                                                                    \
        const float dpre = (q->pre1 - q->pre0) / (float)n, ca = q->col_a, kz = q->col_kz, gfb = q->gfb;       \
        const float g1 = CP(DG1), g2 = CP(DG2), g3 = CP(DG3), g4 = CP(DG4);                                   \
        const int32_t bpd = RV.base[L_PD], b1 = RV.base[L_D1], b2 = RV.base[L_D2], b3 = RV.base[L_D3];        \
        const int32_t b4 = RV.base[L_D4], n1 = RV.dd[0], n2 = RV.dd[1], n3 = RV.dd[2], n4 = RV.dd[3];         \
        const int hp = q->col_hp, m4 = q->mode4, still = dpre == 0.f && pre == (float)(int32_t)pre;           \
        const uint32_t sd = (w + (uint32_t)(b1 - RV.len[L_D1])) & m, sw = (w + (uint32_t)bpd) & m;            \
        const uint32_t sr = (w + (uint32_t)(bpd - (int32_t)pre)) & m;                                         \
        if (still && sd + (uint32_t)(b4 - b1 + RV.len[L_D1] + n) < M && sw + (uint32_t)n <= M && sr + (uint32_t)n <= M) {\
            T *const pd = rg + sd + RV.len[L_D1];   /* (D1's write position; the others at fixed offsets) */  \
            T *const pw = rg + sw;                                                                            \
            const T *const pp = rg + sr;                                                                      \
            const int32_t o2 = b2 - b1, o3 = b3 - b1, o4 = b4 - b1;                                           \
            for (i = 0; i < n; i++) {                                                                         \
                float x = xin[i] * (SC), u, v, d, dq = 0.f;                                                   \
                T *const q_ = pd + i;                                                                         \
                if ((SC) != 1.f) { qr = qr * 1664525u + 1013904223u; dq = RVQ_DQ(qr); }                       \
                (void)dq;                                                                                     \
                if (ca > 0.f) { const float c_ = ca * (x - lpz), y_ = c_ + lpz; lpz = y_ + kz * c_; x = hp ? x - y_ : y_; }\
                d = RP(pp + i);                                                                               \
                v = x + gfb * d;                                                                              \
                if (RVQ_PDZ && (SC) != 1.f && gfb != 0.f && sy_absf(v) < RVQ_PDZ) *(pw + i) = (T)(int32_t)v; else WP(pw + i, v); \
                u = m4 ? v : d;                                                                               \
                d = RP(q_ - n1); v = u + g1 * d; WP(q_, v); u = d - g1 * v;                                   \
                d = RP(q_ + o2 - n2); v = u + g2 * d; WP(q_ + o2, v); u = d - g2 * v;                         \
                d = RP(q_ + o3 - n3); v = u + g3 * d; WP(q_ + o3, v); u = d - g3 * v;                         \
                d = RP(q_ + o4 - n4); v = u + g4 * d; WP(q_ + o4, v); u = d - g4 * v;                         \
                U[i] = u;                                                                                     \
            }                                                                                                 \
        } else                                                                                                \
        for (i = 0; i < n; i++) {                                                                             \
            float x = xin[i] * (SC), u, v, d, dq = 0.f;                                                       \
            if ((SC) != 1.f) { qr = qr * 1664525u + 1013904223u; dq = RVQ_DQ(qr); }                           \
            (void)dq;                                                                                         \
            if (ca > 0.f) { const float c_ = ca * (x - lpz), y_ = c_ + lpz; lpz = y_ + kz * c_; x = hp ? x - y_ : y_; }\
            if (still) d = RD(rg, ww + bpd - (int32_t)pre);                                                   \
            else {                                                                                            \
                float fr; int32_t mi;                                                                         \
                pre += dpre; mi = (int32_t)pre; fr = pre - (float)mi;                                         \
                d = RD(rg, ww + bpd - mi); d += fr * (RD(rg, ww + bpd - mi - 1) - d);                         \
            }                                                                                                 \
            v = x + gfb * d;                                                                                  \
            if (RVQ_PDZ && (SC) != 1.f && gfb != 0.f && sy_absf(v) < RVQ_PDZ) rg[(ww + bpd) & m] = (T)(int32_t)v; else WR(rg, ww + bpd, v); \
            u = m4 ? v : d;                                                                                   \
            d = RD(rg, ww + b1 - n1); v = u + g1 * d; WR(rg, ww + b1, v); u = d - g1 * v;                     \
            d = RD(rg, ww + b2 - n2); v = u + g2 * d; WR(rg, ww + b2, v); u = d - g2 * v;                     \
            d = RD(rg, ww + b3 - n3); v = u + g3 * d; WR(rg, ww + b3, v); u = d - g3 * v;                     \
            d = RD(rg, ww + b4 - n4); v = u + g4 * d; WR(rg, ww + b4, v); u = d - g4 * v;                     \
            U[i] = u;                                                                                         \
            ww++;                                                                                             \
        }                                                                                                     \
        RV.qr = qr; RV.lpz = lpz;                                                                             \
    }                                                                                                         \
    {   /* A: the lines' ends */                                                                              \
        float mm0[2], dmm[2];                                                                                 \
        const int32_t L = q->lfo_l;                                                                           \
        int pr;                                                                                               \
        SY_FAR(rv_mod_block)(q, n, mm0, dmm, rv_cs);                                                          \
        for (l = 2 * (1 + RV_MOD2); l < RV_N; l++) {                                                          \
            const uint32_t s0 = (w + (uint32_t)(RV.base[L_F0 + l] - RV.fl[l])) & m;                          \
            float *const o = RVO(l);                                                                          \
            if (s0 + (uint32_t)n <= M) { const T *p = rg + s0; for (i = 0; i < n; i++) o[RVOS * i] = RP(p + i); } \
            else for (i = 0; i < n; i++) o[RVOS * i] = RD(rg, s0 + (uint32_t)i);                               \
        }                                                                                                     \
        for (pr = 0; pr < 1 + RV_MOD2; pr++) {                                                                \
            /* a pair: line A's delay fl + m, line B's fl + (depth - m), the same weights mirrored */         \
            const int32_t eA = RV.base[L_F0 + 2 * pr] - RV.fl[2 * pr], eB = RV.base[L_F0 + 2 * pr + 1] - RV.fl[2 * pr + 1]; \
            float *const oA = RVO(2 * pr), *const oB = RVO(2 * pr + 1);                                       \
            float f = 0.f, wt[NW], mv = mm0[pr];                                                              \
            const float dm = dmm[pr];                                                                         \
            int32_t ia = 0, ib = 0, k, j;                                                                     \
            for (i = 0; i < n; i += (LST)) {    /* ((LST) frames a step: the weights every (LST) frames, steps of 0.01 \
                                         * frame at most, far below hearing; the step's words in a run, else   \
                                         * gathered first) */                                                  \
                T ta[(LST) - 1 + NW], tb[(LST) - 1 + NW];                                                                     \
                const T *pa, *pb;                                                                             \
                f = mv; ia = (int32_t)f; f -= (float)ia; ib = L - ia - 1;                                     \
                LAGW(NW, f, wt)                                                                               \
                {   const uint32_t sa = (w + (uint32_t)(i + eA - ia + NW / 2 - NW)) & m;                      \
                    const uint32_t sb = (w + (uint32_t)(i + eB - ib - NW / 2)) & m;                           \
                    if (sa + ((LST) - 1 + NW) <= M) pa = rg + sa + (NW - 1);                                          \
                    else { for (k = 0; k < (LST) - 1 + NW; k++) ta[k] = rg[(sa + (uint32_t)k) & m]; pa = ta + (NW - 1); } \
                    if (sb + ((LST) - 1 + NW) <= M) pb = rg + sb;                                                     \
                    else { for (k = 0; k < (LST) - 1 + NW; k++) tb[k] = rg[(sb + (uint32_t)k) & m]; pb = tb; }        \
                }                                                                                             \
                for (j = 0; j < (LST); j++) {                                                                     \
                    float ya = wt[0] * RP(pa), yb = wt[0] * RP(pb);                                           \
                    for (k = 1; k < NW; k++) { ya += wt[k] * RP(pa - k); yb += wt[k] * RP(pb + k); }          \
                    oA[RVOS * (i + j)] = ya; oB[RVOS * (i + j)] = yb;                                           \
                    mv += dm; pa++; pb++;                                                                     \
                }                                                                                             \
            }                                                                                                 \
        }                                                                                                     \
    }                                                                                                         \
    if (RV_SPLIT) {     /* B (RV_SPLIT) */                                                                    \
        RV_MIXC(rv_mix1)(n, yr, yl, rv_cs); RV_MIXC(rv_mix2)(n, yr, yl, rv_cs, (&CP(GM0))[RV.room] * (1.f / (SC))); \
    }                                                                                                         \
    else {   /* B: the late outputs, the mixing (three short loops over the block: each keeps few values live; the first
         * stage and the second of lines 0 - 3, the same of 4 - 7 with the outputs, then the third stage) */        \
        const float gm = (&CP(GM0))[RV.room] * (1.f / (SC));                                                 \
        float cs0 = rv_cs[0], cs1 = rv_cs[1], cs2 = rv_cs[2], cs3 = rv_cs[3];                                 \
        const float dc0 = rv_cs[4], dc1 = rv_cs[5], dc2 = rv_cs[6], dc3 = rv_cs[7];                           \
        float *const O0 = RVO(0), *const O1 = RVO(1), *const O2 = RVO(2), *const O3 = RVO(3);                 \
        float *const O4 = RVO(4), *const O5 = RVO(5), *const O6 = RVO(6), *const O7 = RVO(7);                 \
        for (i = 0; i < n; i++) {                                                                             \
            const float z0 = O0[i], z1 = O1[i], z2 = O2[i], z3 = O3[i];                                      \
            float a2, a3;                                                                                     \
            const float a0 = z0 + z1, a1 = z0 - z1;                                                           \
            yr[i] = z0 - z2; yl[i] = z1 - z3;                                                                 \
            if (RV_ROTON) { a2 = cs0 * z2 + cs1 * z3; a3 = cs1 * z2 - cs0 * z3; cs0 += dc0; cs1 += dc1; }     \
            else { a2 = z2 + z3; a3 = z2 - z3; }                                                              \
            O0[i] = a0 + a2; O2[i] = a0 - a2; O1[i] = a1 + a3; O3[i] = a1 - a3;                               \
        }                                                                                                     \
        for (i = 0; i < n; i++) {                                                                             \
            const float z4 = O4[i], z5 = O5[i], z6 = O6[i], z7 = O7[i];                                      \
            float a6, a7;                                                                                     \
            const float a4 = z4 + z5, a5 = z4 - z5;                                                           \
            yr[i] = gm * (yr[i] + (z4 - z6)); yl[i] = gm * (yl[i] + (z5 - z7));                               \
            if (RV_ROTON) { a6 = cs2 * z6 + cs3 * z7; a7 = cs3 * z6 - cs2 * z7; cs2 += dc2; cs3 += dc3; }     \
            else { a6 = z6 + z7; a7 = z6 - z7; }                                                              \
            O4[i] = a4 + a6; O6[i] = a4 - a6; O5[i] = a5 + a7; O7[i] = a5 - a7;                               \
        }                                                                                                     \
        for (i = 0; i < n; i++) {                                                                             \
            const float c0 = O0[i], c1 = O1[i], c2 = O2[i], c3 = O3[i], c4 = O4[i], c5 = O5[i], c6 = O6[i], c7 = O7[i]; \
            O0[i] = c0 + c4; O4[i] = c0 - c4; O1[i] = c1 + c5; O5[i] = c1 - c5;                               \
            O2[i] = c2 + c6; O6[i] = c2 - c6; O3[i] = c3 + c7; O7[i] = c3 - c7;                               \
        }                                                                                                     \
    }                                                                                                         \
    {   /* C: the lines written through their absorption, the taps */                                                                  \
        const float t0 = (&CP(TAP0))[RV.room] * (1.f / (SC)), er = (&CP(ER))[RV.room] * (1.f / (SC));       \
        float *const DQ = rv_dq;                                                                              \
        if ((SC) != 1.f) {              /* (the dither: a value a frame, the same for every line's write) */  \
            uint32_t qr = RV.qr;                                                                              \
            for (i = 0; i < n; i++) { qr = qr * 1664525u + 1013904223u; DQ[i] = RVQ_DQ(qr); }                 \
            RV.qr = qr;                                                                                       \
        }                                                                                                     \
        for (ph = 0; ph < 2; ph++) {    /* (the lines whose diffused input is added, then, with it turned negative, the\
                                         * rest: no multiply by a sign; + - + + - + - -) */                    \
            if (ph) for (i = 0; i < n; i++) U[i] = -U[i];                                                     \
            for (l = 0; l < RV_N; l++) {                                                                      \
                const float *const o = RVO(l);                                                                \
                const float bb = q->b[l], pp = q->p[l];                                                       \
                uint32_t s = (w + (uint32_t)RV.base[L_F0 + l]) & m;                                           \
                float z = RV.z[l];                                                                            \
                float dq = 0.f;                                                                               \
                int i0 = 0;                                                                                   \
                (void)dq;                                                                                     \
                if ((int)((0xd2u >> l) & 1u) != ph) continue;                                                 \
                while (i0 < n) {        /* (a run to the ring's end, then the rest from its start) */         \
                    const int nn = (uint32_t)(n - i0) < M - s ? n - i0 : (int)(M - s);                        \
                    T *const p = rg + s;                                                                      \
                    const float *const oo = o + i0, *const uu = U + i0, *const dd = DQ + i0;                  \
                    (void)dd;                                                                                 \
                    if (RV_SPLIT) z = (l < 4 ? CWP : CWM)(p, RVO(l & 3) + RVOS * i0, RVO((l & 3) + 4) + RVOS * i0, uu, dd, nn, \
                                                         bb, pp, z);                                           \
                    else for (i = 0; i < nn; i++) { if ((SC) != 1.f) dq = dd[i]; z = bb * oo[i] + pp * z; WP(p + i, z + uu[i]); } \
                    s = (s + (uint32_t)nn) & m; i0 += nn;                                                     \
                }                                                                                             \
                RV.z[l] = z;                                                                                  \
            }                                                                                                 \
        }                                                                                                     \
        {   int side;               /* a side's taps: y += +-g x the first tap's run + e x (the two early taps' \
                                         * runs' sum), each run a stretch of the ring below the write position;  \
                                         * the first tap + right, - left (RV_ETR's note) */                     \
            for (side = 0; side < 2; side++) {                                                                \
                float *const Y = side ? yl : yr;                                                              \
                const float ta = side ? -t0 : t0;                                                             \
                const int32_t ea = RV.base[L_F0 + side] - (side ? RV.tl : RV.tr);                             \
                const int32_t eb = RV.base[L_F0 + 2 + side] - (side ? RV.etl[0] : RV.etr[0]);                 \
                const int32_t ec = RV.base[L_F0 + 4 + side] - (side ? RV.etl[1] : RV.etr[1]);                 \
                int i0 = 0;                                                                                   \
                while (i0 < n) {                                                                              \
                    const uint32_t sa = (w + (uint32_t)ea + (uint32_t)i0) & m, sb = (w + (uint32_t)eb + (uint32_t)i0) & m; \
                    const uint32_t sc = (w + (uint32_t)ec + (uint32_t)i0) & m;                                \
                    uint32_t run = M - sa;                                                                    \
                    const T *const pa = rg + sa, *const pb = rg + sb, *const pc = rg + sc;                    \
                    float *const Yy = Y + i0;                                                                 \
                    int nn;                                                                                   \
                    if (M - sb < run) run = M - sb;                                                           \
                    if (M - sc < run) run = M - sc;                                                           \
                    nn = (uint32_t)(n - i0) < run ? n - i0 : (int)run;                                        \
                    for (i = 0; i < nn; i++) Yy[i] += ta * RP(pa + i) + er * (RP(pb + i) + RP(pc + i));       \
                    i0 += nn;                                                                                 \
                }                                                                                             \
            }                                                                                                 \
        }                                                                                                     \
    }                                                                                                         \
    RV.w = w + (uint32_t)n;                                                                                   \
}
static float rv_cs[8];                  /* (the mixing's two turned pairs this block: sqrt 2 (cos, sin) each at its
                                         * start, then their steps a frame) */
#define WPF(p, v) (*(p) = (v))
#if HYB_RV_BITS == 16 && !defined(HYB_RV_F16)
#define WP16(p, v) (*(p) = q16(v, dq))
#elif HYB_RV_BITS == 16
#define WP16(p, v) (*(p) = f2h(v))
#endif

RV_CW_FN(rv_cwp_f, float, WPF, 1.f, +)
RV_CW_FN(rv_cwm_f, float, WPF, 1.f, -)
RV_NET_FN(rv_net_f, float, RDF, WRF, RPF, WPF, RV_NWF, 1.f, RV_LST, rv_cwp_f, rv_cwm_f)               /* FULL */
#if HYB_RV_BITS == 32 || RV_HALF_F
RV_NET_FN(rv_net_fh, float, RDF, WRF, RPF, WPF, RV_NWH, 1.f, RV_LSTH, rv_cwp_f, rv_cwm_f)             /* HALF, a float ring */
#endif
#if HYB_RV_BITS == 16
RV_CW_FN(rv_cwp_16, RV_W16, WP16, RVQ_ONE, +)
RV_CW_FN(rv_cwm_16, RV_W16, WP16, RVQ_ONE, -)
#if !RV_HALF_F
RV_NET_FN(rv_net_h, RV_W16, RD16, WR16, RP16, WP16, RV_NWH, RVQ_ONE, RV_LSTH, rv_cwp_16, rv_cwm_16)   /* HALF, 16-bit */
#endif
RV_NET_FN(rv_net_hf, RV_W16, RD16, WR16, RP16, WP16, RV_NWF, RVQ_ONE, RV_LST, rv_cwp_16, rv_cwm_16)  /* FULL, 16-bit */
#endif

#if RV_SLOT
typedef void rv_net_t(const rv_par *q, const float *xin, float *yl, float *yr, int n);
static uint32_t rv_slot[RV_SLOT_BYTES / 4u] __attribute__((section(".rt_spare"), aligned(4)));   /* (the variant in use) */
static uint8_t rv_slot_v;               /* the variant in rv_slot: 0 none, 1 rv_net_f, 2 rv_net_h (RV_HALF_F: rv_net_fh),
                                         * 3 rv_net_hf */
#if RV_HALF_F
#define rv_net_hv rv_net_fh             /* (HALF's variant: on the float ring) */
#else
#define rv_net_hv rv_net_h
#endif
/* (rv_path, at a layout change: the variant for RV.w16 / RV.full into rv_slot; the bytes after a shorter one's end are
 * the flash's next code, copied along and never run) */
static __attribute__((noinline)) SY_COLD void rv_slot_load(void)
{
    const int v = RV.w16 ? (RV.full ? 3 : 2) : RV_HALF_F && !RV.full ? 2 : 1;
    const volatile uint32_t *src;
    uint32_t i;
    if (v == rv_slot_v) return;
    src = (const volatile uint32_t *)(uintptr_t)(v == 1 ? (void *)SY_FAR(rv_net_f) : v == 2 ? (void *)SY_FAR(rv_net_hv)
                                                                                            : (void *)SY_FAR(rv_net_hf));
    for (i = 0; i < RV_SLOT_BYTES / 4u; i++) rv_slot[i] = src[i];
    __asm__ volatile("csync" ::: "memory");   /* (the writes done before the slot's code is fetched) */
    rv_slot_v = (uint8_t)v;
}
#endif

/* ---- the block ---- */
#define RV_CLR 4096u                    /* ring words cleared a block (the reverb silent meanwhile) */
SY_WORK_ATTR static float rv_x[SY_BLOCK], rv_yl[SY_BLOCK], rv_yr[SY_BLOCK];
/* HALF's buffers (the decimator's input, the half-rate input and outputs, the interpolators' inputs, the shimmer's
 * signal): static, not on the stack. The effects run on the second core (SY_FX_PIPE), whose stack is 2816 B with the
 * trap 256 B above its bottom: these 2.2 KB in hyb_rv_run's frame (rv_run inlined) put the chain sy_fx_render ->
 * hyb_rv_run -> rv_net_* at ~3.1 KB, a CPU1 fault and a restart at the first block with signal (1.18C3) */
SY_WORK_ATTR static float rv_hb[HBN + SY_BLOCK], rv_xh[SY_BLOCK / 2];
SY_WORK_ATTR static float rv_bl[HBN / 2 + SY_BLOCK / 2], rv_br[HBN / 2 + SY_BLOCK / 2];   /* (the half-rate outputs are written at 11: the
                                         * interpolators' history in front, no copy) */
#ifdef HYB_SHIMMER
SY_WORK_ATTR static float rv_sh[SY_BLOCK / 2];
#endif

static int rv_words(void) { return RV.half ? 16384 : 32768; }

RVCOLD int rv_clear_step(void)          /* a part of the ring cleared; 1 when done */
{
    const uint32_t n = (uint32_t)rv_words() * (RV.w16 ? 2u : 4u) / 4u;   /* (32-bit words) */
    uint32_t *p = (uint32_t *)(void *)rv_ring, i, e, c = RV_CLR;
    if (!RV.clear) return 1;
#if RV_HALF_F
    if (!RV.w16 && RV.half) c = 2u * RV_CLR;   /* (HALF's float ring, 64 KB, in the 2 blocks the 16-bit HALF's 32 KB took:
                                         * the network silent as long, its positions and LFO where 1.18C40 had them) */
#endif
    i = (uint32_t)(RV.clear - 1) * c;
    e = i + c < n ? i + c : n;
    for (; i < e; i++) p[i] = 0u;
    if (e >= n) { RV.clear = 0; return 1; }
    RV.clear++;
    return 0;
}

RVCOLD void rv_states_clear(void)
{
    int i;
    RV.lpz = 0.f;
    for (i = 0; i < RV_N; i++) RV.z[i] = 0.f;
    for (i = 0; i < HBN; i++) RV.dz[i] = 0.f;
    for (i = 0; i < HBN / 2; i++) RV.uz[0][i] = RV.uz[1][i] = 0.f;
#if RV_LF
    rvlf_clear();
#endif
#if RVQ_FADE
    RV.qf = 0;                          /* (a layout's change, a rest, the gate's clear: no fade) */
#endif
}

#if RVQ_FADE
static float rvq_lvl(int k)             /* the fade's gain after k of RVQ_FADE blocks: -RVQ_FADE_DB (1 - cos(pi k / N)) / 2 */
{
    const float c = FCOS2PI((float)k * (0.5f / (float)RVQ_FADE));
    return FEXP2(-RVQ_FADE_DB * 0.16609640f * 0.5f * (1.f - c));   /* (log2(10) / 20) */
}
/* (RVQ_FADE above) a block with RV.qf set, after the block's wet gain g (from g + dg to g + SY_BLOCK dg over the block)
 * is made: the gain times the fade's from this block's start to its end; the clear after the last; the take-back */
RVCOLD void rvq_fade(const uint8_t *p, float *g, float *dg)
{
    const float g0 = *g, g1 = *g + (float)SY_BLOCK * *dg;
    float f0, f1;
    if (RV.qdone != 3 || p[116] >= 127) {   /* (a sound at the input, decay 127: the level back within this block) */
        f0 = rvq_lvl(RV.qf - 1); f1 = 1.f;
        RV.qf = 0;
        if (RV.qdone == 3) RV.qdone = 0;
    } else if (RV.qf > RVQ_FADE) {     /* (faded out: the gate's clear, as 1.18C40's) */
        f0 = f1 = 0.f;
        RV.qdone = 2; RV.clear = 1;
        rv_states_clear();              /* (RV.qf 0) */
    } else {
        f0 = rvq_lvl(RV.qf - 1); f1 = rvq_lvl(RV.qf);
        RV.qf++;
    }
    *g = g0 * f0;
    *dg = (g1 * f1 - *g) * (1.f / (float)SY_BLOCK);
}
#endif

#ifdef HYB_SHIMMER
static int rvs_wants_half(const uint8_t *p);
RVR __attribute__((noinline)) int rvs_block(const rv_par *q, float t60, float *s);
RVR __attribute__((noinline)) void rvs_after(const float *xh, const float *lh, const float *rh, float t60);
#endif
/* (1.18C47) FX > SHIMMER's WIDTH (the prototype's variant C1): the wet's side narrowed while
 * the input sounds, widened as the tail rings out, the middle kept; W GLIDE the time of that change. 0: none of it is
 * compiled (1.18C46's engine) */
#ifndef RVS_WIDTH
#define RVS_WIDTH 1
#endif
#if !defined(HYB_SHIMMER)
#undef RVS_WIDTH
#define RVS_WIDTH 0
#endif
#if RVS_WIDTH
#define RVS_WGD 5u                      /* W GLIDE's default (1 s): the controls' byte 6 holds TAIL | (W GLIDE ^ 5) << 1,
                                         * so a byte of before (bits 1-4 zero) reads the default */
#ifndef RVS_WIN
#define RVS_WIN 1e-4f                   /* (the input sounds: its 30 ms energy above -40 dB of the output's 20 ms, as
                                         * RVS_LEVEL's test) */
#endif
#ifndef RVS_WID
#define RVS_WID 0.1f                    /* (... and within 10 dB of its own peak: the input falling away is its end) */
#endif
#ifndef RVS_WIF
#define RVS_WIF 3.f                     /* (dB/s: that peak's fall; a held note decaying slower counts as sounding) */
#endif
#ifndef RVS_WCN
#define RVS_WCN 0.6f                    /* the side x (1 - 0.6 W) while the input sounds (WIDTH 127: W 1) */
#endif
#ifndef RVS_WCW
#define RVS_WCW 0.25f                   /* the side x (1 + 0.25 W) rung out (the mono sum's late loss <= ~1 dB) */
#endif
#ifndef RVS_WT0
#define RVS_WT0 0.5f                    /* (s) W GLIDE 0's glide; each step x 2^(1/5): 1 s at 5 (the default), 4 s at 15 */
#endif
#ifndef RVS_WSL
#define RVS_WSL 2.6f                    /* (/s) the side's gain moves at most this fast (a WIDTH turned, the shimmer
                                         * coming on or off): the fastest glide's own steepest (0.85 x 1.5 / 0.5 s = 2.55)
                                         * stays under it */
#endif
RVCOLD void rvs_c1(float *lh, float *rh);
#endif

#ifndef RV_SW_FADE
#define RV_SW_FADE 8                    /* (after 1.18C32) a change of the path (FULL / HALF: REVERB, AUTO, the overload
                                         * guard): the tail faded out over these blocks on the old layout (8: ~22 ms; 1:
                                         * a block, as 1.18C32: on a loud, dark tail a step heard as a click) */
#endif
/* the path for this block: 1 run (rv_ring, the layout set), 2 run a block on the old layout (a change: fading out,
 * RV_SW_FADE blocks, RV.quiet the block of it), 0 silent (the ring being cleared) */
#if SY_BLK_RAM2
#define RV_B2 static SY_B2M             /* (1.18C37: once a block, in RAM; its rare calls into the flash far) */
#define RVF(fn) SY_FAR(fn)
#else
#define RV_B2 RVCOLD
#define RVF(fn) fn
#endif
RV_B2 int rv_path(float *ring, const uint8_t *p)
{
    int half = 1, room = RVF(rv_room)(p), w16 = HYB_RV_BITS == 16;
    float *rg = ring;
#ifdef HYB_RV_SWITCH
    half = RV.want_half;
#endif
#ifdef HYB_SHIMMER
    if (RVF(rvs_wants_half)(p)) half = 1;
#endif
#if HYB_RV_BITS == 16
    {
        float *f = 0;                   /* (FULL on Bluetooth's lent float ring, else on the 16-bit ring: its 32768
                                         * words hold FULL's lines too) */
#ifdef HYB_RV_LEND
        f = rv_lent;
#endif
        if (!half && f) { rg = f; w16 = 0; }
#if RV_HALF_F
        if (half) w16 = 0;              /* (HALF: 16384 floats on the 16-bit ring's 64 KB) */
#endif
    }
#endif
    if (RV.key && !RV.clear && RV.quiet < RV_SW_FADE && (half != RV.half || w16 != RV.w16)) {
        RV.quiet++;                     /* (this block fades the tail out on the old layout, rv_ring as it was) */
        return 2;
    }
    if (RV.quiet && half == RV.half && w16 == RV.w16) RV.quiet = 0;   /* (the change taken back while it faded: the
                                         * next one fades in full; the tail comes back at once) */
    rv_ring = rg;
    if (!RV.key || half != RV.half || w16 != RV.w16) {
        RV.quiet = 0;
        RV.w16 = (uint8_t)w16;
        RVF(rv_layout)(half);
        RV.mask = (uint32_t)RVF(rv_words)() - 1u;
        RV.full = (uint8_t)!half;
#if RV_SLOT
        RVF(rv_slot_load)();                 /* (this block clears: the network does not run until the clear is done) */
#endif
        RV.key = 1;
        if (!RV.phs) { RV.ph = CP(LFOPH); RV.phs = 1; }
        RV.clear = 1;
        RV.qdone = 1;
        RV.qfade = 0;                   /* (RVF_GATE: a fade of the old layout's tail ends with it) */
        RVF(rv_states_clear)();
    }
    if (room != RV.room) RVF(rv_rooms)(room);
    if (RV.clear) { RVF(rv_clear_step)(); return 0; }
    return 1;
}

static float rv_glide_k(void)           /* the pre-delay's glide a block (the network's blocks: 128 frames at 46,875) */
{
    return 1.f - sy_exp2(-1.442695f * (float)SY_BLOCK / (CP(GLIDE) * RVF_RATE));
}

/* ---- the low-frequency path (RV_LF, DESIGN, rvbal; PROVENANCE.md section 17): below ~220 Hz the wet is a bank of
 * resonators whose frequencies and weights are FITTED to the reference's measured wet LF transfer per room (black-box
 * renders, cleaner/tools/measure/reverb/fit_rvlf.py), their decay the network's T60. The network's input is high-passed
 * (4th-order Butterworth, 220 Hz); the bank takes the input decimated to 732 Hz (CIC^3 / 16, a 6th-order Butterworth
 * low pass at 260 Hz, / 4), its own colour (one pole), pre-delay and feedback echo at that rate; its outputs are
 * interpolated back (x 4 through the same low pass, then straight lines) and added to the network's before the wet
 * gain. Two LF samples a block; the per-block work in the flash (RVCOLD), the room's modes copied to RAM ---- */
#if RV_LF
#include "sy_cln_rvlf.h"
#define RVLF_PD 512u                    /* (the LF pre-delay's line: 23533 / 64 frames at the most) */
typedef struct {
    float m[RVLF_NMAX][8];              /* per mode: the pole (re, im), the weights (left re, im, right re, im), the
                                         * state (re, im) */
#if RVLF_FAST
    float cic[4][3];                    /* (the CIC's stages' last three inputs, the oldest first) */
#else
    float xh[45 + SY_BLOCK];            /* (the CIC's history, then the block) */
#endif
    float aa[3][2], ip[2][3][2], hp[2][2];   /* (the low passes' and the high pass' states, TDF II) */
    float last[2][3];                   /* (the interpolators' last samples: the oldest first; [k][2] the last) */
    float pd[RVLF_PD];
    float cz, ca, cg;                   /* the colour: state, coefficient (0 off), the high pass' gain */
    int chp;                            /* (the colour a high pass) */
    float gfb;                          /* (the feedback echo) */
    uint32_t pw;
    int n, m4, room, on;
#if RVLF_FAST
    float seg[2][11];                   /* (this block's output at the x 4 rate, a side: the last block's three last
                                         * points, then the 8 new ones; hyb_rv_run draws the straight lines between
                                         * seg[2..10] or the spline over seg[0..10]) */
#endif
} rvlf_t;
SY_WORK_ATTR static rvlf_t RVL;
#if RVLF_FAST
static inline __attribute__((always_inline))
#else
static
#endif
float rvlf_bq(const float *c, float *z, float x)   /* (one biquad, transposed direct form II) */
{
    const float y = c[0] * x + z[0];
    z[0] = c[1] * x - c[3] * y + z[1];
    z[1] = c[2] * x - c[4] * y;
    return y;
}
RVCOLD void rvlf_clear(void)            /* the states 0, the parameters kept (in place: the modes' 2.2 KB copied to the
                                         * stack and back put the second core's chain at ~3 KB, past its stack's trap:
                                         * after 1.18C30, stack_check.py's tail calls) */
{
    uint32_t i;
    for (i = 0; i < (uint32_t)RVLF_NMAX; i++) RVL.m[i][6] = RVL.m[i][7] = 0.f;
#if RVLF_FAST
    for (i = 0; i < 12u; i++) (&RVL.cic[0][0])[i] = 0.f;
#else
    for (i = 0; i < sizeof(RVL.xh) / sizeof(RVL.xh[0]); i++) RVL.xh[i] = 0.f;
#endif
    for (i = 0; i < 6u; i++) (&RVL.aa[0][0])[i] = 0.f;
    for (i = 0; i < 12u; i++) (&RVL.ip[0][0][0])[i] = 0.f;
    for (i = 0; i < 4u; i++) (&RVL.hp[0][0])[i] = 0.f;
    for (i = 0; i < 6u; i++) (&RVL.last[0][0])[i] = 0.f;
    for (i = 0; i < RVLF_PD; i++) RVL.pd[i] = 0.f;
    RVL.cz = 0.f;
    RVL.pw = 0u;
}
/* the parameters (with rv_par_make's): the room's modes, their pole radius from the T60, the colour, the feedback */
RVCOLD void rvlf_par(const rv_par *q, const uint8_t *p)
{
    const float fs = RVF_RATE / 64.f;
    const int room = RV.room;
    const float r = q->t60 < 0.f ? 0.9999999f : FEXP2(-9.965784f / (fs * q->t60));   /* (-60 dB in T60) */
    const float d = (float)p[116];
    float sc = RVLF_SC[room] * (1.f / 32767.f), c = (float)p[119], fc;
    int k;
    /* the level by decay (FITTED: the gains at the decay nodes, straight between, held outside) */
    for (k = 0; k < 4 && d > (float)RVLF_DN[k + 1]; k++) ;
    if (d <= (float)RVLF_DN[0]) sc *= RVLF_G[room][0];
    else if (k >= 4) sc *= RVLF_G[room][4];
    else sc *= RVLF_G[room][k] + (d - (float)RVLF_DN[k]) / (float)(RVLF_DN[k + 1] - RVLF_DN[k]) * (RVLF_G[room][k + 1] - RVLF_G[room][k]);
    RVL.n = RVLF_N[room]; RVL.room = room; RVL.on = RVL.n > 0;
#ifdef RV_LF_TEST
    RVL.on = 1;                         /* (a measurement build: the bank a plain path, the network silent) */
#endif
    for (k = 0; k < RVL.n; k++) {
        const float w = (float)RVLF_F[room][k] * (1.f / 65536.f);
        RVL.m[k][0] = r * FCOS2PI(w); RVL.m[k][1] = r * FSIN2PI(w);
        RVL.m[k][2] = sc * (float)RVLF_W[room][k][0]; RVL.m[k][3] = sc * (float)RVLF_W[room][k][1];
        RVL.m[k][4] = sc * (float)RVLF_W[room][k][2]; RVL.m[k][5] = sc * (float)RVLF_W[room][k][3];
    }
    /* the colour at 732 Hz: the trapezoidal one pole with g = pi fc / fs, unwarped (its corner may lie above this
     * rate's Nyquist frequency; below 300 Hz the analog one pole's response within ~1 dB); a low pass above 3 kHz off */
    RVL.ca = 0.f; RVL.chp = 0; RVL.cg = 1.f;
    fc = c < 63.5f ? FEXP2(CP(COL_LP0) + c / 63.f * (CP(COL_LP1) - CP(COL_LP0)))
                   : FEXP2(CP(COL_HP0) + (c - 65.f) / 62.f * (CP(COL_HP1) - CP(COL_HP0)));
    if ((c < 63.5f && fc < 3000.f) || c > 64.5f) {
        const float g = SY_PI * fc / fs;
        RVL.ca = g / (1.f + g); RVL.chp = c > 64.5f;
    }
    RVL.gfb = q->gfb; RVL.m4 = q->mode4;
}
/* the network's input high-passed (FULL: the block at 46,875 Hz, HALF: its half-rate input) */
RVCOLD SY_BLKR void rvlf_hp(float *x, int n, int half)
{
    const float *c0 = half ? RVLF_HPH[0] : RVLF_HPF[0], *c1 = half ? RVLF_HPH[1] : RVLF_HPF[1];
    const float a0 = c0[0], a1 = c0[1], a2 = c0[2], a3 = c0[3], a4 = c0[4];
    const float b0 = c1[0], b1 = c1[1], b2 = c1[2], b3 = c1[3], b4 = c1[4];
    float z0 = RVL.hp[0][0], z1 = RVL.hp[0][1], w0 = RVL.hp[1][0], w1 = RVL.hp[1][1];
    int i;
    if (!RVL.on) return;
    for (i = 0; i < n; i++) {           /* (the two sections inline, their states in registers) */
        const float x0 = x[i], y0 = a0 * x0 + z0;
        float y1;
        z0 = a1 * x0 - a3 * y0 + z1; z1 = a2 * x0 - a4 * y0;
#if RVLF_FAST
        /* (the second section's numerator 1, -2, 1 as numbers: the same products, three multiplies and three
         * registers fewer) */
        (void)b0; (void)b1; (void)b2;
        y1 = y0 + w0;
        w0 = -2.f * y0 - b3 * y1 + w1; w1 = y0 - b4 * y1;
#else
        y1 = b0 * y0 + w0;
        w0 = b1 * y0 - b3 * y1 + w1; w1 = b2 * y0 - b4 * y1;
#endif
        x[i] = y1;
    }
    RVL.hp[0][0] = z0; RVL.hp[0][1] = z1; RVL.hp[1][0] = w0; RVL.hp[1][1] = w1;
}
#if RVLF_FAST
/* one stage of the decimator: n inputs to n / 2 outputs, y[m] = x[2m] + 3 (x[2m - 1] + x[2m - 2]) + x[2m - 3] (the
 * CIC's (1 + 1/z + .. + 1/z^15)^3 = the product of (1 + 1/z^k)^3, k = 1, 2, 4, 8: four such stages, each at half the
 * rate of the one before; h the stage's last three inputs) */
static inline __attribute__((always_inline)) void rvlf_cic2(float *h, const float *x, float *y, int n)
{
    float t3 = h[0], t2 = h[1], t1 = h[2];
    int m;
    for (m = 0; m < n / 2; m += 2) {    /* (two outputs a turn: no moves of the history) */
        const float x0 = x[2 * m], x1 = x[2 * m + 1], x2 = x[2 * m + 2], x3 = x[2 * m + 3];
        y[m] = (x0 + t3) + 3.f * (t1 + t2);
        y[m + 1] = (x2 + t1) + 3.f * (x1 + x0);
        t3 = x1; t2 = x2; t1 = x3;
    }
    h[0] = t3; h[1] = t2; h[2] = t1;
}
/* the bank (RVLF_FAST): both LF samples u in one pass over the modes, each mode's arithmetic and the sums' order as one
 * sample at a time (the same numbers); y[side][sample]. A function of its own: its loop alone in the registers */
RVCOLD SY_BLKR void rvlf_bank(const float *u, float (*y)[2])
{
    const float u0 = u[0], u1 = u[1];
    float l0 = 0.f, r0 = 0.f, l1 = 0.f, r1 = 0.f;
    float *m = RVL.m[0], *const me = RVL.m[RVL.n];   /* (a pointer to the end: no counter) */
    NO_UNROLL for (; m != me; m += 8) {
        const float p0 = m[0], p1 = m[1];
        const float a = p0 * m[6] - p1 * m[7] + u0, b = p0 * m[7] + p1 * m[6];
        const float a2 = p0 * a - p1 * b + u1, b2_ = p0 * b + p1 * a;
        l0 += m[2] * a + m[3] * b; r0 += m[4] * a + m[5] * b;
        l1 += m[2] * a2 + m[3] * b2_; r1 += m[4] * a2 + m[5] * b2_;
        m[6] = a2; m[7] = b2_;
    }
    y[0][0] = l0; y[1][0] = r0; y[0][1] = l1; y[1][1] = r1;
}
/* a block (RVLF_FAST): x the reverb's input at 46,875 Hz (before the high pass), pre the pre-delay (frames at 46,875
 * Hz); the output at the x 4 rate into RVL.seg (hyb_rv_run adds the straight lines between) */
RVCOLD SY_BLKR void rvlf_run(const float *x, float pre)
{
    float *const b1 = rv_yl, *const b2 = rv_yl + SY_BLOCK / 2;   /* (the decimator's stages: rv_yl is free until
                                         * rv_run, not on the second core's small stack) */
    float u2[8], u[2], y[2][2];
    int j, k;
    uint32_t dl;
    if (!RVL.on) {
        for (j = 0; j < 22; j++) (&RVL.seg[0][0])[j] = 0.f;
        return;
    }
    rvlf_cic2(RVL.cic[0], x, b1, SY_BLOCK);   /* (CIC^3 / 16 in four stages, then the low pass at 2.93 kHz) */
    rvlf_cic2(RVL.cic[1], b1, b2, SY_BLOCK / 2);
    rvlf_cic2(RVL.cic[2], b2, b1, SY_BLOCK / 4);
    rvlf_cic2(RVL.cic[3], b1, b2, SY_BLOCK / 8);
    {
        float *const a0 = RVL.aa[0], *const a1 = RVL.aa[1], *const a2 = RVL.aa[2];
        for (j = 0; j < 8; j++)
            u2[j] = rvlf_bq(RVLF_AA[2], a2, rvlf_bq(RVLF_AA[1], a1, rvlf_bq(RVLF_AA[0], a0, b2[j] * (1.f / 4096.f))));
    }
    dl = (uint32_t)(pre * (1.f / 64.f) + 0.5f);
    if (dl < 1u) dl = 1u;
    if (dl > RVLF_PD - 2u) dl = RVLF_PD - 2u;
    for (j = 0; j < 2; j++) {           /* (/ 4: the colour, the pre-delay with the feedback echo) */
        float v = u2[4 * j + 3], d;
        if (RVL.ca > 0.f) { const float c_ = RVL.ca * (v - RVL.cz), y_ = c_ + RVL.cz; RVL.cz = y_ + c_; v = RVL.chp ? RVL.cg * (v - y_) : y_; }
        d = RVL.pd[(RVL.pw - dl) & (RVLF_PD - 1u)];
        v += RVL.gfb * d;
        RVL.pd[RVL.pw & (RVLF_PD - 1u)] = v;
        RVL.pw++;
        u[j] = RVL.m4 ? v : d;
    }
#ifdef RV_LF_TEST
    y[0][0] = y[1][0] = u[0]; y[0][1] = y[1][1] = u[1];
#else
    SY_FAR(rvlf_bank)(u, y);
#endif
    for (k = 0; k < 2; k++) {           /* (x 4 through the low pass: the straight lines / 16 in hyb_rv_run) */
        float *const ipz = &RVL.ip[k][0][0], *const sg = RVL.seg[k];
        sg[0] = RVL.last[k][0]; sg[1] = RVL.last[k][1]; sg[2] = RVL.last[k][2];
        for (j = 0; j < 8; j++) {
            const float z = (j & 3) == 0 ? 4.f * y[k][j >> 2] : 0.f;
            sg[j + 3] = rvlf_bq(RVLF_AA[2], ipz + 4, rvlf_bq(RVLF_AA[1], ipz + 2, rvlf_bq(RVLF_AA[0], ipz, z)));
        }
        RVL.last[k][0] = sg[8]; RVL.last[k][1] = sg[9]; RVL.last[k][2] = sg[10];
    }
}
#else
/* a block: rvlf_in the reverb's input at 46,875 Hz (before the high pass), rvlf_block the bank's output added to yl,
 * yr; pre the pre-delay (frames at 46,875 Hz) */
RVCOLD void rvlf_in(const float *x)
{
    int i;
    for (i = 0; i < SY_BLOCK; i++) RVL.xh[45 + i] = x[i];
}
RVCOLD void rvlf_block(float *yl, float *yr, float pre)
{
    float u2[8], u[2], y[2][2];
    int i, j, k;
    uint32_t dl;
    if (!RVL.on) return;
    for (j = 0; j < 8; j++) {           /* (CIC^3 / 16, then the low pass at 2.93 kHz) */
        const float *z = RVL.xh + 16 * j;   /* (z[45] = x[16 j]) */
        float a = 0.f;
        NO_UNROLL for (k = 0; k < 46; k++) a += RVLF_CIC[k] * z[k];
        u2[j] = rvlf_bq(RVLF_AA[2], RVL.aa[2], rvlf_bq(RVLF_AA[1], RVL.aa[1], rvlf_bq(RVLF_AA[0], RVL.aa[0], a)));
    }
    for (i = 0; i < 45; i++) RVL.xh[i] = RVL.xh[SY_BLOCK + i];
    dl = (uint32_t)(pre * (1.f / 64.f) + 0.5f);
    if (dl < 1u) dl = 1u;
    if (dl > RVLF_PD - 2u) dl = RVLF_PD - 2u;
    for (j = 0; j < 2; j++) {           /* (/ 4: the colour, the pre-delay with the feedback echo) */
        float v = u2[4 * j + 3], d;
        if (RVL.ca > 0.f) { const float c_ = RVL.ca * (v - RVL.cz), y_ = c_ + RVL.cz; RVL.cz = y_ + c_; v = RVL.chp ? RVL.cg * (v - y_) : y_; }
        d = RVL.pd[(RVL.pw - dl) & (RVLF_PD - 1u)];
        v += RVL.gfb * d;
        RVL.pd[RVL.pw & (RVLF_PD - 1u)] = v;
        RVL.pw++;
        u[j] = RVL.m4 ? v : d;
    }
    for (j = 0; j < 2; j++) {           /* (the bank) */
        float l = 0.f, r = 0.f;
        const float uu = u[j];
#ifdef RV_LF_TEST
        l = r = uu; (void)k;
#else
        NO_UNROLL for (k = 0; k < RVL.n; k++) {
            float *const m = RVL.m[k];
            const float a = m[0] * m[6] - m[1] * m[7] + uu, b = m[0] * m[7] + m[1] * m[6];
            m[6] = a; m[7] = b;
            l += m[2] * a + m[3] * b; r += m[4] * a + m[5] * b;
        }
#endif
        y[0][j] = l; y[1][j] = r;
    }
    for (k = 0; k < 2; k++) {           /* (x 4 through the low pass, then / 16 straight lines) */
        float *const Y = k ? yr : yl, *const ipz = &RVL.ip[k][0][0];
        float prev = RVL.last[k][2];
        for (j = 0; j < 8; j++) {
            const float z = (j & 3) == 0 ? 4.f * y[k][j >> 2] : 0.f;
            const float cur = rvlf_bq(RVLF_AA[2], ipz + 4, rvlf_bq(RVLF_AA[1], ipz + 2, rvlf_bq(RVLF_AA[0], ipz, z)));
            const float d = (cur - prev) * (1.f / 16.f);
            NO_UNROLL for (i = 0; i < 16; i++) Y[16 * j + i] += prev + d * (float)(i + 1);
            prev = cur;
        }
        RVL.last[k][2] = prev;
    }
}
#endif
#if RVLF_FAST && RVLF_SPLINE
#ifdef __clang__
#define RVLF_UNROLL4 _Pragma("clang loop unroll_count(4)")   /* (the device's compiler unrolls the 16 frames fully:
                                                            * 486 B of RAM code; by 4: 284 B, ~1 instruction more
                                                            * per 4 frames) */
#else
#define RVLF_UNROLL4
#endif
/* segment j of a side's LF output (RVLF_SPLINE): the cubic B-spline over q = seg + j (q[0..3]), its 16 frames at
 * t = 1/16 .. 1 added to y[0..15]: the first value and its forward differences (v0, v1, v2, v3; then v0 += v1,
 * v1 += v2, v2 += v3). (1.18C38: the frames added here, once a segment and side, instead of in hyb_rv_run's
 * output loop: there the two sides' 8 values did not fit the registers beside the mix's and were kept on the stack,
 * +15 instructions a frame; here 4 live values. The same operations in the same order, the same output) */
RVR static __attribute__((noinline)) void rvlf_cub(const float *q, float *y)   /* (one copy: RAM) */
{
    const float h = 1.f / 16.f, h2 = h * h, h3 = h2 * h;
    const float c0 = (q[0] + 4.f * q[1] + q[2]) * (1.f / 6.f), c1 = (q[2] - q[0]) * 0.5f;
    const float c2 = (q[0] + q[2]) * 0.5f - q[1], c3 = (3.f * (q[1] - q[2]) + q[3] - q[0]) * (1.f / 6.f);
    float v0 = c0 + h * (c1 + h * (c2 + h * c3));
    float v1 = c1 * h + 3.f * c2 * h2 + 7.f * c3 * h3;
    float v2 = 2.f * c2 * h2 + 12.f * c3 * h3;
    const float v3 = 6.f * c3 * h3;
    int i;
    RVLF_UNROLL4 for (i = 0; i < 16; i++) { y[i] += v0; v0 += v1; v1 += v2; v2 += v3; }
}
#endif
static int rvlf_still(void)             /* (hyb_rv_still: the bank's states and the lines at rest) */
{
    uint32_t i;
    const float *p = (const float *)(const void *)&RVL;
    if (!RVL.on) return 1;
    for (i = 0; i < (uint32_t)RVL.n; i++) if (sy_absf(RVL.m[i][6]) > 1e-12f || sy_absf(RVL.m[i][7]) > 1e-12f) return 0;
    for (i = (uint32_t)(sizeof(RVL.m) / 4u); i < (uint32_t)((const float *)(const void *)&RVL.ca - p); i++)   /* (the
                                         * filters' states, the lines, the colour's state: up to the parameters) */
        if (sy_absf(p[i]) > 1e-12f) return 0;
    return 1;
}
#endif

/* HALF's interpolator, a side: the half-rate output (written at b + HBN / 2, its last HBN / 2 frames kept in z) to
 * the engine's rate (a function of its own: one loop in RAM for both sides) */
RVR static __attribute__((noinline)) void rv_hb_up(float *b, float *z, float *y)
{
    int j;
    for (j = 0; j < HBN / 2; j++) b[j] = z[j];
    for (j = 0; j < SY_BLOCK / 2; j++) {
        const float *u = b + j;         /* (u[11] = yh[j]) */
        y[2 * j] = 2.f * (HB[0] * (u[11] + u[0]) + HB[1] * (u[10] + u[1]) + HB[2] * (u[9] + u[2]) + HB[3] * (u[8] + u[3])
                        + HB[4] * (u[7] + u[4]) + HB[5] * (u[6] + u[5]));
        y[2 * j + 1] = u[6];
    }
    for (j = 0; j < HBN / 2; j++) z[j] = b[SY_BLOCK / 2 + j];
}

/* the network over a block of the engine's: FULL straight, HALF between the half-band filters */
RVR static void rv_run(const rv_par *q, const float *x, float *yl, float *yr)
{
#if RV_SLOT
#define RV_NET(a, b, c, n) ((rv_net_t *)(void *)rv_slot)(q, a, b, c, n)   /* (rv_path loaded the layout's variant) */
#elif RV_HALF_F
#define RV_NET(a, b, c, n) (RV.w16 ? rv_net_hf : RV.full ? rv_net_f : rv_net_fh)(q, a, b, c, n)   /* (HALF on its
                                         * float ring, FULL on the lent float ring or the 16-bit one) */
#elif HYB_RV_BITS == 16
#define RV_NET(a, b, c, n) (RV.w16 ? (RV.full ? rv_net_hf : rv_net_h) : rv_net_f)(q, a, b, c, n)   /* (HALF is
                                         * always 16-bit here: no float HALF loop) */
#else
#define RV_NET(a, b, c, n) (RV.full ? rv_net_f : rv_net_fh)(q, a, b, c, n)
#endif
#if RV_LF
    if (RV.full) { SY_FAR(rvlf_hp)((float *)(uintptr_t)x, SY_BLOCK, 0); RV_NET(x, yl, yr, SY_BLOCK); return; }
#else
    if (RV.full) { RV_NET(x, yl, yr, SY_BLOCK); return; }
#endif
    {
        float *const xh = rv_xh, *const lh = rv_bl + HBN / 2, *const rh = rv_br + HBN / 2, *const hb = rv_hb;
        int j;
        /* decimate: xh[j] = sum h[k] x[2j - k] (hb: the last 14 inputs, then the block's) */
        for (j = 0; j < HBN; j++) hb[j] = RV.dz[j];
        if (x != hb + HBN) for (j = 0; j < SY_BLOCK; j++) hb[HBN + j] = x[j];   /* (hyb_rv_run: there already) */
        for (j = 0; j < SY_BLOCK / 2; j++) {
            const float *z = hb + 2 * j;   /* (z[22] = x[2j]) */
            xh[j] = HB[0] * (z[0] + z[22]) + HB[1] * (z[2] + z[20]) + HB[2] * (z[4] + z[18]) + HB[3] * (z[6] + z[16])
                  + HB[4] * (z[8] + z[14]) + HB[5] * (z[10] + z[12]) + 0.5f * z[11];
        }
        for (j = 0; j < HBN; j++) RV.dz[j] = hb[SY_BLOCK + j];
#if RV_LF
        SY_FAR(rvlf_hp)(xh, SY_BLOCK / 2, 1);
#endif
#ifdef HYB_SHIMMER
        {
            float *const sh = rv_sh;
            const int on = SY_FAR(rvs_block)(q, q->t60, sh);
            if (on) for (j = 0; j < SY_BLOCK / 2; j++) sh[j] += xh[j];
            RV_NET(on ? sh : xh, lh, rh, SY_BLOCK / 2);
            if (on || SY_FAR(hyb_rv_shim_on)()) SY_FAR(rvs_after)(xh, lh, rh, q->t60);
#if RVS_WIDTH
            SY_FAR(rvs_c1)(lh, rh);     /* (WIDTH: the flash, once a block, after the loop took its input; at rest
                                         * with WIDTH 0 it returns at once) */
#endif
        }
#else
        RV_NET(xh, lh, rh, SY_BLOCK / 2);
#endif
        /* interpolate: y[2j] = 2 sum h[2i] yh[j - i] (12 taps), y[2j + 1] = yh[j - 5] (bl, br: the last 11, then the
         * block's) */
        rv_hb_up(rv_bl, RV.uz[0], yl);
        rv_hb_up(rv_br, RV.uz[1], yr);
    }
#undef RV_NET
}

RVR int hyb_rv_run(float *ring, const uint8_t *p, int mode, float *l, float *r, float dry, float wet, float gin)
{
    rv_par q;
    float *xd;
    int i, st;
    const float k = RV.half ? 0.5f : 1.f;
#if HYB_RV_BITS == 16 && !defined(HYB_RV_F16)
    if (RV.qdone == 2 && RV.clear) {   /* (the gate's clear stopped by a sound: the rest of the ring is at the floor
                                         * already; none of the input dropped) */
        for (i = 0; i < SY_BLOCK; i += 8) if ((l[i] != 0.f) | (r[i] != 0.f)) { RV.clear = 0; RV.qdone = 0; break; }
    }
#endif
    st = SY_FAR(rv_path)(ring, p);
    float g, dg = 0.f;
    if (!st) {
        for (i = 0; i < SY_BLOCK; i++) { l[i] *= dry; r[i] *= dry; }
        return 1;
    }
    {   /* the parameters made again only when what they depend on changed (rv_par_make: ~600 instructions) */
        const uint32_t k0 = (uint32_t)p[114] | (uint32_t)p[115] << 8 | (uint32_t)p[116] << 16 | (uint32_t)p[117] << 24;
        const uint32_t k1 = (uint32_t)p[118] | (uint32_t)p[119] << 8 | (uint32_t)mode << 16 | (uint32_t)RV.room << 24
                          | (uint32_t)RV.half << 28 | (uint32_t)RV.w16 << 29 | 1u << 31;
        if (k0 != rv_qk[0] || k1 != rv_qk[1] || hyb_rv_clk != rv_qk[2]) {
            SY_FAR(rv_par_make)(&rv_qc, p, mode);
            rv_qk[0] = k0; rv_qk[1] = k1; rv_qk[2] = hyb_rv_clk;
        }
        q = rv_qc;
    }
    if (RV.pre <= 0.f) RV.pre = q.pre1 / k;   /* (the first block: no glide from nothing) */
    q.pre0 = RV.pre * k;
    RV.pre += (q.pre1 / k - RV.pre) * SY_FAR(rv_glide_k)();
    q.pre1 = RV.pre * k;
    g = 0.5f * gin;
    xd = RV.full ? rv_x : rv_hb + HBN;   /* (HALF: straight into the decimator's buffer) */
    for (i = 0; i < SY_BLOCK; i++) xd[i] = (l[i] + r[i]) * g;
#if RVF_GATE
    if (!RV.w16) {                      /* (the float ring's rest: RVF_GATE above; the count in the flash, rvf_gate) */
        float a = 0.f;
        for (i = 0; i < SY_BLOCK; i++) a = sy_maxf(a, sy_absf(xd[i]));
        if (a < RVF_IN) SY_FAR(rvf_gate)(p, a);
        else if (RV.qdone | RV.qfade | RV.qlow) { RV.qdone = 0; RV.qlow = 0; RV.qfade = 0; hyb_rv_arm = 0; }
    }
#endif
#if RV_LF
#if RVLF_FAST
    SY_FAR(rvlf_run)(xd, RV.pre);       /* (before rv_run: FULL high-passes xd in place) */
#else
    SY_FAR(rvlf_in)(xd);
#endif
#endif
    rv_run(&q, xd, rv_yl, rv_yr);
#if RV_LF
#ifdef RV_LF_TEST
    for (i = 0; i < SY_BLOCK; i++) rv_yl[i] = rv_yr[i] = 0.f;
#endif
#if !RVLF_FAST
    SY_FAR(rvlf_block)(rv_yl, rv_yr, RV.pre);
#endif
#endif
#if HYB_RV_BITS == 16 && !defined(HYB_RV_F16)
    if (RV.w16) {                       /* (a limit cycle's end, q16: every eighth frame looked at) */
        float a = 0.f, b = 0.f;
#if RV_LF && RVLF_FAST
        for (i = 0; i < SY_BLOCK; i += 8) {   /* (the LF path's straight lines added as the output loop adds them) */
            const int j = i >> 4;
            const float fi = (float)((i & 15) + 1);
#if RVLF_SPLINE
            /* (the spline's points as straight lines: a peak's estimate for the gate, within its 3 steps) */
            const float pl = RVL.seg[0][j + 1], dl = (RVL.seg[0][j + 2] - pl) * (1.f / 16.f);
            const float pr = RVL.seg[1][j + 1], dr = (RVL.seg[1][j + 2] - pr) * (1.f / 16.f);
#else
            const float pl = RVL.seg[0][j + 2], dl = (RVL.seg[0][j + 3] - pl) * (1.f / 16.f);
            const float pr = RVL.seg[1][j + 2], dr = (RVL.seg[1][j + 3] - pr) * (1.f / 16.f);
#endif
            a = sy_maxf(a, sy_absf(xd[i]));
            b = sy_maxf(b, sy_maxf(sy_absf(rv_yl[i] + (pl + dl * fi)), sy_absf(rv_yr[i] + (pr + dr * fi))));
        }
#else
        for (i = 0; i < SY_BLOCK; i += 8) { a = sy_maxf(a, sy_absf(xd[i])); b = sy_maxf(b, sy_maxf(sy_absf(rv_yl[i]), sy_absf(rv_yr[i]))); }
#endif
        /* (held as long as the pre-delay too: a sound on its way through it, or the feedback's next repeat, is
         * not yet at the output) */
        /* (once a silence: cleared, the ring stays clean until the input sounds again; a fresh layout's ring is
         * clean too) */
        if (a * RVQ_ONE >= 0.5f) RV.qdone = 0;
        RV.qlow = a * RVQ_ONE < 0.5f && b * RVQ_ONE < q.gate && p[116] < 127 && !RV.qdone ? (uint16_t)(RV.qlow + 1) : 0;
        if (RV.qlow >= RVQ_HOLD + (int)(RV.pre * (1.f / (float)SY_BLOCK)) + 1) {
#if RVQ_FADE
            RV.qlow = 0; RV.qdone = 3; RV.qf = 1;   /* (the fade first: rvq_fade, then the clear) */
#else
            RV.qlow = 0; RV.qdone = 2; RV.clear = 1; SY_FAR(rv_states_clear)();
#endif
        }
    }
#endif
    g = wet * CP(GAIN) * (&CP(GR))[RV.room] * q.gl * (RV.half ? CP(GH) : 1.f);
    if (st == 2) {                      /* (a change's blocks: faded out, RV_SW_FADE of them, straight) */
        dg = -g * (1.f / (float)(RV_SW_FADE * SY_BLOCK));
        g *= (float)(RV_SW_FADE + 1 - RV.quiet) * (1.f / (float)RV_SW_FADE);
    }
#if RVF_GATE
    else if (RV.qfade) {                /* (the float ring's rest: its fade, straight, RVF_FADE blocks) */
        dg = -g * (1.f / (float)(RVF_FADE * SY_BLOCK));
        g *= (float)(RVF_FADE + 1 - RV.qfade) * (1.f / (float)RVF_FADE);
    }
#endif
#if RVQ_FADE
    if (RV.qf) SY_FAR(rvq_fade)(p, &g, &dg);   /* (the 16-bit gate's fade, or its take-back) */
#endif
#if RV_LF && RVLF_FAST && RVLF_SPLINE
    {   /* the LF path's output: the spline's 16 frames a segment added to rv_yl, rv_yr (rvlf_cub; the same sums as
         * the fix's output loop made, YL[i] + v0), then the plain output loop below */
        int j;
        NO_UNROLL for (j = 0; j < 8; j++) { rvlf_cub(RVL.seg[0] + j, rv_yl + 16 * j); rvlf_cub(RVL.seg[1] + j, rv_yr + 16 * j); }
    }
#endif
#if RV_LF && RVLF_FAST && !RVLF_SPLINE
    {   /* the LF path's output: straight lines / 16 between its points at the x 4 rate, added here (as rvlf_block
         * added them to rv_yl, rv_yr: the same sums) */
        int j;
        float *L = l, *R = r;
        const float *YL = rv_yl, *YR = rv_yr;
        for (j = 0; j < 8; j++) {
            const float pl = RVL.seg[0][j + 2], dl = (RVL.seg[0][j + 3] - pl) * (1.f / 16.f);
            const float pr = RVL.seg[1][j + 2], dr = (RVL.seg[1][j + 3] - pr) * (1.f / 16.f);
            float fi = 1.f;
            for (i = 0; i < 16; i++) {
                g += dg;
                L[i] = dry * L[i] + g * (YL[i] + (pl + dl * fi));
                R[i] = dry * R[i] + g * (YR[i] + (pr + dr * fi));
                fi += 1.f;
            }
            L += 16; R += 16; YL += 16; YR += 16;
        }
    }
#else
    for (i = 0; i < SY_BLOCK; i++) {
        g += dg;
        l[i] = dry * l[i] + g * rv_yl[i];
        r[i] = dry * r[i] + g * rv_yr[i];
    }
#endif
    return 1;
}

#ifdef HYB_SHIMMER
static void rvs_off(void);
#endif
void hyb_rv_reset(void)
{
    RV.key = 0;                        /* (the next block: the layout made again, the ring cleared) */
    RV.pre = 0.f;
}
void hyb_rv_clear(void)                 /* (1.18C49, a firmware's PANIC and ENGINE's way back: app.c fx_clear) the ring
                                         * as hyb_rv_reset, and the shimmer's buffer and its loop: cleared as it comes on
                                         * again */
{
    hyb_rv_reset();
#ifdef HYB_SHIMMER
    rvs_off();
#endif
}

int hyb_rv_health(uint32_t *h)
{
    int i;
    for (i = 0; i < 8; i++) h[i] = 0u;
    return 0;
}

/* a block of zeros skipped (sy_fx.c SY_FX_REST): the positions, the LFO and the glide advanced as hyb_rv_run would */
void hyb_rv_quiet(float *ring, const uint8_t *p, int mode)
{
    rv_par q;
    const int n = RV.half ? SY_BLOCK / 2 : SY_BLOCK;
    float k;
    if (SY_FAR(rv_path)(ring, p) != 1) return;
    k = RV.half ? 0.5f : 1.f;
    SY_FAR(rv_par_make)(&q, p, mode);
    if (RV.pre <= 0.f) RV.pre = q.pre1 / k;
    RV.pre += (q.pre1 / k - RV.pre) * SY_FAR(rv_glide_k)();
    RV.w += (uint32_t)n;
    {   float m0[2], dm[2], cs[8];
        SY_FAR(rv_mod_block)(&q, n, m0, dm, cs);
    }
}

int hyb_rv_still(float *ring, int tiny)  /* the ring and the states all 0 (tiny: or subnormal, then set to 0) */
{
    const uint32_t *u = (const uint32_t *)(const void *)rv_ring;
    uint32_t n, i, o = 0u;
    if (!RV.key || RV.clear || !rv_ring) return 1;
#if RV_LF
    if (!rvlf_still()) return 0;
#endif
#ifdef HYB_SHIMMER
    if (hyb_rv_shim_on()) return 0;     /* (the shimmer running: no rest, its grains move by themselves) */
#endif
    n = (uint32_t)rv_words() * (RV.w16 ? 2u : 4u) / 4u;
#if RVF_GATE
    if (!RV.w16 && RV.qdone == 2 && RV.qz) n = 0u;   /* (the float ring's gate cleared it and nothing but zeros came in
                                         * since: no need to look at its 32 768 words, a block's worth of the core) */
#endif
#if HYB_RV_BITS == 16
    if (RV.w16) {                       /* (16-bit words: 0 when a word is +-0) */
        for (i = 0; i < n; i++) o |= u[i];
        if (o) return 0;
    } else
#endif
    for (i = 0; i < n; i++) {
        const uint32_t x = u[i];
        if (tiny ? (x & 0x7f800000u) != 0u : (x << 1) != 0u) return 0;
        o |= x;
    }
    {
        const float st[1 + RV_N] = {RV.lpz, RV.z[0], RV.z[1], RV.z[2], RV.z[3], RV.z[4], RV.z[5], RV.z[6], RV.z[7]};
        for (i = 0; i < 1u + RV_N; i++) {
            sy_fu f; f.f = st[i];
            if (tiny ? (f.u & 0x7f800000u) != 0u : (f.u << 1) != 0u) return 0;
        }
        for (i = 0; i < (uint32_t)HBN; i++) if (RV.dz[i] != 0.f) { sy_fu f; f.f = RV.dz[i]; if (!tiny || (f.u & 0x7f800000u)) return 0; }
        for (i = 0; i < (uint32_t)HBN / 2u; i++) if (RV.uz[0][i] != 0.f || RV.uz[1][i] != 0.f) return 0;
    }
    if (o << 1) for (i = 0; i < n; i++) ((uint32_t *)(void *)rv_ring)[i] = 0u;
    rv_states_clear();
    return 1;
}

#ifdef HYB_RV_SWITCH
void hyb_rv_set_full(int full) { RV.want_half = (uint8_t)(full == 0); }
int hyb_rv_is_full(void) { return RV.key ? RV.full : !RV.want_half; }
#endif
#ifdef HYB_RV_LEND
void hyb_rv_lend(float *ring) { rv_lent = ring; RV.key = 0; }
#endif

#ifdef HYB_SHIMMER
/* ---- FX > SHIMMER (this project's own effect, its reimplementation on this reverb) ----
 * The reverb's output (HALF, 23,437.5 Hz), mono, through TONE (two one-pole low passes) and a 150 Hz high pass into a
 * buffer of 16384 floats; one or two granular pitch shifters (two Hann grains half a period apart each, linear
 * interpolation, a random extra delay at each grain's start; the second layer's grains 0.79 as long) read it, and
 * their sum goes back into the reverb's input with the loop gain sqrt(lambda / E): lambda = 0.9 x AMOUNT / 127 x the
 * list's factor, E the reverb's energy gain (its output's energy for a unit of energy in) at its room and decay.
 * Bounded: lambda < 1, a governor on the block's peak, the injection clamped at +-0.25; at decay 127 off. TAIL
 * LIMITED: the gain x (energy / peak)^1.5 of the reverb's output, its peak falling at SP_TLR x the reverb's decay rate;
 * SWELL: after an onset of the input the gain ramps in over SP_SWT x (v / 127)^2. The block runs before the network's
 * (its reads at least a block old), the network's output written after. Numbers: DESIGN (the shimmer's own, from
 * its design notes) or FITTED to the development engine's shimmer as a black box (fit_shim.py) */
float hyb_rv_shm[3];
uint8_t hyb_rv_shl3;
#define RVS_N 16384u                    /* the buffer (floats) */
#define RVS_BLK (SY_BLOCK / 2)
enum { SP_FAC1, SP_FAC2, SP_FAC3, SP_FAC4, SP_FAC5, SP_EK0, SP_EK1, SP_EK2, SP_EK3, SP_RND, SP_DMIN, SP_GOV, SP_GUP,
       SP_TLK, SP_SWT,                 /* TAIL LIMITED's gain x, SWELL's ramp at 127 (s) (FITTED: fit_shim.py tail) */
       SP_TLR,                         /* TAIL LIMITED: its peak's fall, x the reverb's decay rate (FITTED: fit_shim.py
                                        * tail, the shimmer's level at 4-8 s against 1-4 s) */
       SP_N };
#ifdef CLN_RV_TUNE
float cln_rvs_tune[16] = {
#else
static const float cln_rvs_tune[SP_N] = {
#endif
/* FIT BEGIN (apply_fit.py, fit_shim.py) */
    1.f, 0.8629913f, 0.8917345f, 0.8220582f,   /* FAC1 FAC2 FAC3 FAC4 */
    0.5254156f, 22.77316f, 16.83172f, 13.28846f,   /* FAC5 EK0 EK1 EK2 */
    5.234439f, 0.25f, 80.f, 0.25f,   /* EK3 RND DMIN GOV */
    0.07f, 1.1f, 4.f, 0.5f,   /* GUP TLK SWT TLR */
/* FIT END */
};
#define SP(k) cln_rvs_tune[SP_##k]
typedef struct {
    float ph[2], dly[2][2];             /* the layers' grain phases; each grain's extra delay */
    float lp1, lp2, hp;                 /* TONE, the high pass */
    float gov, es, pk, sf, ss, sw;      /* the governor; TAIL's follower and peak; SWELL's followers and factor */
    uint32_t rnd, w;
    uint8_t v[HYB_SHIM_N];              /* the controls as given */
    uint8_t on, clr, amt, tone;         /* running; the buffer being cleared (quarters + 1); the loop's AMOUNT, TONE */
    float gain;                         /* the loop gain last block (a host test's look) */
    float tls, tlg, tln;                /* (1.18C45, RVS_LEVEL) TAIL LIMITED's factor's mean; the input's presence; the
                                         * blocks in that mean */
    float mdep, mm;                     /* (1.18C45) MODULATION: its depth now (cents, smoothed); the common delay M / rho
                                         * at the last block's end (the reverb's LFO) */
    uint8_t mod, mact, marm, mcom;            /* MODULATION as the loop takes it (0..127); running (the grains' headroom
                                         * given); the blocks since it came on (RVS_ARM: no pitch change before); the
                                         * last block's by the common delay (1) or per grain (0, LFO 3) */
#if RVS_WIDTH
    float wst;                          /* (1.18C47) W GLIDE's step a block (a whole glide RVS_WT0 x 2^(k / 5) s, k
                                         * 0..15); 0: not set yet (the default's) */
    float wp, wd, wi;                   /* WIDTH: the glide's place (0 narrow .. 1 wide); the side's gain - 1 last block
                                         * (0 at rest: the memory's zeros are a rest); the input's energy's peak */
#endif
} rvs_t;
#ifdef HYB_SHIM_LEND
#define RVS (*(rvs_t *)(void *)((char *)rv_lent - HYB_RV_LEND_PRE))   /* (the radio's memory below the lent ring) */
#define RVS_BUF rv_lent
#define RVS_OK (rv_lent != 0)
typedef char rvs_fits[sizeof(rvs_t) <= HYB_RV_LEND_PRE ? 1 : -1];
static uint8_t rvs_req[HYB_SHIM_N];     /* (the controls, kept while not lent) */
#else
static rvs_t rvs_s;
#define RVS rvs_s
#define RVS_BUF (rv_ring + 16384)        /* (the float ring's second 64 KB: HALF uses the first) */
#define RVS_OK 1
#endif

static inline __attribute__((always_inline)) int rvs_amount(void)
{
#ifdef HYB_SHIM_LEND
    const uint8_t *v = rvs_req;
#else
    const uint8_t *v = RVS.v;
#endif
    int a = v[0];
    if (!a) return 0;
    a += sy_floori(hyb_rv_shm[0] + 0.5f);
    return a < 1 ? 1 : a > 127 ? 127 : a;
}
static int rvs_wants_half(const uint8_t *p)   /* the shimmer on: AMOUNT > 0 in a reverb mode (HALF then) */
{
    (void)p;
    return RVS_OK && rvs_amount() > 0;
}
void hyb_rv_shim(const uint8_t *v)
{
    int i;
#ifdef HYB_SHIM_LEND
    for (i = 0; i < HYB_SHIM_N; i++) rvs_req[i] = v[i];
    if (!RVS_OK) return;
#endif
    for (i = 0; i < HYB_SHIM_N; i++) RVS.v[i] = v[i];
#if RVS_WIDTH
    {   /* (TAIL; bits 1-4 W GLIDE's k, as RVS_WGD's offset: 0 its default): the glide's step a block */
        float t = (float)RVS_BLK / (RVF_RATE * 0.5f) * (1.f / RVS_WT0);
        for (i = (int)((v[6] >> 1 & 15u) ^ RVS_WGD); i > 0; i--) t *= 0.87055056f;   /* (2^(-1/5)) */
        RVS.v[6] = (uint8_t)(v[6] & 1u);
        RVS.wst = t;
    }
#endif
}
int hyb_rv_shim_on(void) { return RVS_OK && RVS.on; }
static void rvs_off(void) { if (RVS_OK) RVS.on = 0; }   /* (hyb_rv_reset: rvs_block starts it from a cleared buffer;
                                         * the lent memory's state: only while lent) */
int hyb_rv_shim_get(int k)
{
    if (!RVS_OK) return k == 2 ? 0 : 0;
    switch (k) {
    case 0: return RVS.on ? RVS.amt : 0;
    case 1: return RVS.mod;
    case 2: return RVS.v[0];
    case 3: return RVS.v[1];
    case 4: return (int)(RVS.gain * 1000.f);
    case 5: return RVS.v[6];
    case 6: return RVS.v[7];
    case 7: return (int)(RVS.mdep * 100.f + 0.5f);
    }
    return 0;
}

#ifdef CLN_RVS_LOG
float cln_rvs_cap[RVS_BLK];           /* (the shifters' sum of the last block, before the loop gain: SY_SHCAP) */
void cln_rvs_log(float *o)              /* (a host measurement build: the loop's state after the last block) */
{
    if (!RVS_OK) { o[0] = o[1] = o[2] = o[3] = o[4] = o[5] = o[6] = o[7] = 0.f; return; }
    o[0] = RVS.on ? RVS.gain : 0.f; o[1] = RVS.gov; o[2] = RVS.sw; o[3] = RVS.es; o[4] = RVS.pk;
    o[5] = RVS.mm; o[6] = RVS.tlg; o[7] = RVS.tls;
#if RVS_WIDTH && defined(RVS_WLOG)
    o[5] = 1.f + RVS.wd; o[6] = RVS.wp; o[7] = RVS.ss;   /* (WIDTH: the side's gain, the glide's place, the input's energy: tools/perf/shimwidth.py) */
#endif
}
#endif
static const float RVS_RATIO[5][2] = {{2.f, 0.f}, {2.f, 2.9966142f}, {2.f, 4.f}, {2.f, 0.5f}, {1.4983071f, 0.f}};

/* ---- (1.18C45) MODULATION (control 4): the shifted signal's pitch moved by the reverb's own
 * LFO, +-RVS_MCENT x (m / 127)^2 cents, or by LFO 3 when its destination is SHIM MOD (hyb_rv_shl3). A grain reads at
 * rho x (1 + dev) when its delay gains -rho x dev a frame: the per-sample loop's dm (one add a frame; dmin at rest).
 * The reverb's LFO: dm is one delay for every grain, M = -rho x dev / dph x C (C the triangle's integral, bounded):
 * a vibrato after the shifters, the grains' phases kept (a sine glides as a sine). LFO 3 (any shape, any rate, its
 * integral unbounded): each grain's own offset, from its start (where its window is 0) on (gs 1: the offset taken
 * out at its start, kept in dly at a block's end): the grains' read rate follows it, its centre stays LAYERS'
 * interval. Every grain starts the headroom Bh later, so neither reads under dmin. Coming on, the pitch waits RVS_ARM
 * blocks (the grains then running started without Bh). In the flash, once a block; MODULATION 0 without a target:
 * none of it runs, the loop adds 0 (1.18C44 to the bit) ---- */
#ifndef RVS_MCENT
#define RVS_MCENT 50.f                  /* the depth at 127 (cents, peak: DESIGN, the user's +-25..50) */
#endif
#ifndef RVS_MPH
#define RVS_MPH 0.25f                   /* the pitch's phase against the reverb LFO's value (cycles: 0 the pitch highest
                                         * where the LFO's triangle is; 0.25, 0.5, 0.75 measured) */
#endif
#define RVS_MDEV 0.02930224f            /* (2^(50 / 1200) - 1: the headroom's deviation, the most RVS_MCENT may be) */
#define RVS_ARM 80                      /* (blocks, 0.22 s: longer than the longest grain, 200 ms) */
static float rvs_tric(float p)          /* the integral of the triangle 2 tri - 1 (-1 .. 1, highest at p = 0.5) from 0:
                                         * -1/8 .. 1/8 (cycles) */
{
    p -= (float)(int32_t)p;
    if (p < 0.f) p += 1.f;
    return p < 0.5f ? 2.f * p * p - p : (3.f - 2.f * p) * p - 1.f;
}
static void rvs_fold(int l)             /* the common delay into the grains' own (going per grain, or off) */
{
    rvs_t *const R = &RVS;
    int k;
    for (k = 0; k < 2; k++) {
        const float r = RVS_RATIO[l][k] * R->mm;
        R->dly[k][0] += r; R->dly[k][1] += r;
    }
    R->mm = 0.f;
}
RVCOLD void rvs_mod(const rv_par *q, int l, float *du, float *bb, float *dz, float *gs)
{
    rvs_t *const R = &RVS;
    int k, m = R->v[4] + sy_floori(hyb_rv_shm[1] + 0.5f);
    const int com = !hyb_rv_shl3;
    float c, x = 0.f, dv, m0 = R->mm, m1 = 0.f;
    m = m < 0 ? 0 : m > 127 ? 127 : m;
    R->mod = (uint8_t)m;
    c = RVS_MCENT * (float)(m * m) * (1.f / 16129.f);
    if (!R->mact) {
        if (!m) return;
        R->mact = 1; R->marm = 0; R->mdep = 0.f; R->mm = m0 = 0.f; R->mcom = (uint8_t)com;
    }
    if (R->marm < RVS_ARM) { R->marm++; c = 0.f; }
    R->mdep += (c - R->mdep) * 0.09f;   /* (~30 ms at a block a 2.73 ms: no zipper while turned) */
    if (!m && R->mdep < 0.01f) { rvs_fold(l); R->mact = 0; R->mdep = 0.f; return; }
    if (R->mcom && !com) { rvs_fold(l); m0 = 0.f; }
    R->mcom = (uint8_t)com;
    dv = sy_exp2(R->mdep * (1.f / 1200.f)) - 1.f;   /* (the deviation at the peak, as a ratio) */
    if (com) {                           /* (M at the block's end: on from its start, drawn to the bounded integral) */
        const float p0 = RV.ph + RVS_MPH, p1 = p0 + q->dph * (float)RVS_BLK, a = -dv / q->dph;
        m1 = m0 + a * (rvs_tric(p1) - rvs_tric(p0));
        m1 += (a * rvs_tric(p1) - m1) * 0.002f;   /* (a depth turned: back to it over ~1.4 s) */
        R->mm = m1;
    } else {
        x = hyb_rv_shm[2];               /* (LFO 3: its output x its amount / 127, -1..1) */
        x = x > 1.f ? 1.f : x < -1.f ? -1.f : x;
    }
    {
        const float G = 0.001f * 20.f * sy_exp2((float)R->v[2] * (3.321928f / 127.f)) * (RVF_RATE * 0.5f), hd = 0.25f / q->dph;
        const float e = com ? (m1 - m0) * (1.f / (float)RVS_BLK) : 1.f - sy_exp2(R->mdep * x * (1.f / 1200.f));
        for (k = 0; k < 2; k++) {        /* (the second layer's grains 0.79 as long, rvs_block) */
            const float rho = RVS_RATIO[l][k], bh = rho * RVS_MDEV * (G * (k ? 0.79f : 1.f) + hd);
            du[k] = rho * e;
            if (com) { dz[k] = SP(DMIN) + rho * m0; bb[k] = bh; gs[k] = 0.f; }
            else bb[k] = SP(DMIN) + bh;
        }
    }
}

/* ---- (1.18C45) RVS_LEVEL: TAIL LIMITED's factor steady while the input sounds. The
 * factor (energy / peak)^1.5 follows the reverb output's energy over 20 ms against its falling peak: a sustained
 * chord's own ripple (its beats, the reverb's rotation) swung it 0.1 .. 1 from block to block, and that came back on
 * the loop gain and again through the loop: 4-6 dB peak to peak on the shimmer's level (TAIL FREE: 2 dB). While
 * the reverb's input sounds (its energy over 30 ms above -40 dB of the output's: tlg, ~30 ms) the factor is its own
 * mean (tls: the running mean from the input's start, then over RVS_TLS_T); without input it is the 20 ms factor as
 * before, so the tail falls as it did. 0: as 1.18C44 ---- */
#ifndef RVS_LEVEL
#define RVS_LEVEL 1
#endif
#ifndef RVS_TLS_T
#define RVS_TLS_T 4.f                   /* (s: DESIGN, measured: 0.5 s left 4 dB, 2 s 2.8 dB, 5 s 2.4 dB, FREE 1.9 dB) */
#endif
#if RVS_LEVEL
RVCOLD float rvs_tl(void)
{
    rvs_t *const R = &RVS;
    const float blk = (float)RVS_BLK / (RVF_RATE * 0.5f), r = R->es / R->pk, f = r * sy_sqrt(r);
    const float ain = 1.f - sy_exp2(-1.442695f * blk / 0.03f), nmax = RVS_TLS_T / blk;
    R->tlg += ((R->ss > 1e-4f * R->es ? 1.f : 0.f) - R->tlg) * ain;
    if (R->tlg < 0.5f) R->tln = 0.f;  /* (no input: the mean starts again at the next) */
    else if (R->tln < nmax) R->tln += 1.f;
    R->tls += (f - R->tls) / (R->tln < 1.f ? 1.f : R->tln);
    return (f + (R->tls - f) * R->tlg) * SP(TLK);
}
#endif

/* before the network's block (HALF): the shifted signal for its input (s, RVS_BLK frames); 0: none this block */
RVR __attribute__((noinline)) int rvs_block(const rv_par *q, float t60, float *s)
{
    float *b = RVS_BUF;
    const uint32_t m = RVS_N - 1u;
    int amt, l, i, k, nl;
    float lam, E, g, gl, pk = 0.f;
    if (!RVS_OK) return 0;
    amt = rvs_amount();
    if (!amt || t60 < 0.f || !RV.half) { RVS.on = 0; return 0; }
    if (!RVS.on) {                      /* (coming on: the buffer cleared a quarter a block, silent meanwhile) */
        RVS.on = 1; RVS.clr = 1; RVS.gov = 1.f; RVS.es = RVS.pk = RVS.sf = RVS.ss = 0.f; RVS.sw = 1.f;
        RVS.lp1 = RVS.lp2 = RVS.hp = 0.f; RVS.ph[0] = 0.f; RVS.ph[1] = 0.25f; RVS.rnd = 0x9e3779b9u;
        RVS.dly[0][0] = RVS.dly[0][1] = RVS.dly[1][0] = RVS.dly[1][1] = 0.f; RVS.mact = 0;
        RVS.tls = 1.f; RVS.tlg = RVS.tln = 0.f;
    }
    RVS.amt = (uint8_t)amt;
    RVS.tone = RVS.v[3];                /* (1.18C45: no target on TONE; SHIM MOD in its place) */
    if (RVS.clr) {
        float *p = b + (uint32_t)(RVS.clr - 1) * (RVS_N / 4u);
        for (i = 0; i < (int)(RVS_N / 4u); i++) p[i] = 0.f;
        RVS.clr = (uint8_t)(RVS.clr >= 4 ? 0 : RVS.clr + 1);
        return 0;
    }
    l = RVS.v[1] > 4 ? 4 : RVS.v[1];
    nl = RVS_RATIO[l][1] > 0.f ? 2 : 1;
    E = (&SP(EK0))[RV.room] * t60;
    lam = 0.9f * (float)amt * (1.f / 127.f) * (&SP(FAC1))[l];   /* (each layer's loop gain: < 1) */
    if (lam > 0.98f) lam = 0.98f;
    g = SY_FAR(sy_sqrt)(lam / E);
    /* TAIL LIMITED: x (energy / peak)^1.5 (RVS_LEVEL: smoothed while the input sounds, rvs_tl) */
    if (!RVS.v[6] && RVS.pk > 0.f) {
#if RVS_LEVEL
        g *= SY_FAR(rvs_tl)();
#else
        const float r = RVS.es / RVS.pk;
        g *= r * SY_FAR(sy_sqrt)(r) * SP(TLK);
#endif
    }
    g *= RVS.sw * RVS.gov;
    RVS.gain = g;
    for (i = 0; i < RVS_BLK; i++) s[i] = 0.f;
    if (g <= 0.f) return 1;
    {
    float mu[2] = {0.f, 0.f}, mb[2], mz[2], mg[2] = {1.f, 1.f};
    mb[0] = mb[1] = mz[0] = mz[1] = SP(DMIN);
    if (RVS.v[4] || hyb_rv_shm[1] != 0.f || RVS.mact) SY_FAR(rvs_mod)(q, l, mu, mb, mz, mg);
    for (k = 0; k < nl; k++) {
        const float rho = RVS_RATIO[l][k];
        const float G = 0.001f * 20.f * sy_exp2((float)RVS.v[2] * (3.321928f / 127.f)) * (RVF_RATE * 0.5f) * (k ? 0.79f : 1.f);
        const float dph = 1.f / G, span = (rho - 1.f) * G, ddm = mu[k], bb = mb[k], dz = mz[k], gs = mg[k];
        float ph = RVS.ph[k], dm = dz;
        for (i = 0; i < RVS_BLK; i++) {
            float acc = 0.f;
            int j;
            /* the window (DESIGN: a smoothstep of the triangle, within 1.2 % of Hann; the second grain's, half a
             * period on, 1 - the first's: their sum 1) */
            const float tr = ph < 0.5f ? 2.f * ph : 2.f - 2.f * ph, w0 = tr * tr * (3.f - 2.f * tr);
            for (j = 0; j < 2; j++) {
                float f = ph + 0.5f * (float)j, d;
                const float wv = j ? 1.f - w0 : w0;
                int32_t di;
                if (f >= 1.f) f -= 1.f;
                d = dm + RVS.dly[k][j] + (span > 0.f ? span * (1.f - f) : -span * f);
                di = (int32_t)d;
                d -= (float)di;
                {
                    const uint32_t o = RVS.w + (uint32_t)i - (uint32_t)di;
                    const float a = b[o & m], c = b[(o - 1u) & m];
                    acc += wv * (a + d * (c - a));
                }
            }
            s[i] += acc;
            ph += dph;
            if (ph >= 1.f) ph -= 1.f;
            dm += ddm;
            /* (a grain starts: its extra delay drawn; MODULATION: its headroom, the loop's delay taken out) */
            if (ph < dph) { RVS.rnd = RVS.rnd * 1664525u + 1013904223u; RVS.dly[k][0] = SP(RND) * G * (float)(RVS.rnd >> 8) * (1.f / 16777216.f) + (bb - gs * dm); }
            else if (ph >= 0.5f && ph - dph < 0.5f) { RVS.rnd = RVS.rnd * 1664525u + 1013904223u; RVS.dly[k][1] = SP(RND) * G * (float)(RVS.rnd >> 8) * (1.f / 16777216.f) + (bb - gs * dm); }
        }
        RVS.ph[k] = ph;
        dm = (dm - dz) * gs;            /* (per grain: the loop's delay into the grains', dm from dmin each block) */
        RVS.dly[k][0] += dm; RVS.dly[k][1] += dm;
    }
    }
    gl = g;
#ifdef CLN_RVS_LOG
    for (i = 0; i < RVS_BLK; i++) cln_rvs_cap[i] = s[i];
#endif
    for (i = 0; i < RVS_BLK; i++) {
        float v = s[i] * gl;
        const float a = sy_absf(v);
        if (a > pk) pk = a;
        s[i] = v > 0.25f ? 0.25f : v < -0.25f ? -0.25f : v;
    }
    /* the governor: down at once to keep the peak at SP(GOV), up over SP(GUP) */
    if (pk * RVS.gov > SP(GOV) * RVS.gov && pk > 0.f) RVS.gov *= SP(GOV) / pk;
    else RVS.gov += (1.f - RVS.gov) * (1.f - sy_exp2(-1.442695f * (float)RVS_BLK / (SP(GUP) * RVF_RATE * 0.5f)));
    (void)q;
    return 1;
}

/* after the network's block: its output into the buffer (TONE, the high pass); TAIL's follower; SWELL's onsets */
RVR __attribute__((noinline)) void rvs_after(const float *xh, const float *lh, const float *rh, float t60)
{
    float *b = RVS_BUF, e = 0.f, ei = 0.f;
    const float fs = RVF_RATE * 0.5f;
    const float a1 = 1.f - sy_exp2(-9.0647202f * 1500.f * sy_exp2((float)RVS.tone * (2.736966f / 127.f)) / fs);
    const float ah = 1.f - sy_exp2(-9.0647202f * 150.f / fs);
    float lp1 = RVS.lp1, lp2 = RVS.lp2, hp = RVS.hp;
    int i;
    if (!RVS_OK || !RVS.on) return;
    for (i = 0; i < RVS_BLK; i++) {
#ifdef RVS_TEST_DRY
        float x = xh[i];                /* (a host measurement build: the network's input, not its output) */
#else
        float x = 0.5f * (lh[i] + rh[i]);
#endif
        e += x * x;
        ei += xh[i] * xh[i];
        lp1 += a1 * (x - lp1); lp2 += a1 * (lp1 - lp2);
        hp += ah * (lp2 - hp);
        b[(RVS.w + (uint32_t)i) & (RVS_N - 1u)] = RVS.clr ? 0.f : lp2 - hp;
    }
    RVS.lp1 = lp1; RVS.lp2 = lp2; RVS.hp = hp;
    RVS.w += RVS_BLK;
    {   /* TAIL: the output's energy smoothed (~20 ms), its peak falling at half the reverb's rate */
        const float blk = (float)RVS_BLK / fs, rate = t60 > 0.f ? 60.f / t60 : 0.f;
        RVS.es += (e - RVS.es) * (1.f - sy_exp2(-1.442695f * blk / 0.02f));
        RVS.pk *= sy_exp2(-SP(TLR) * rate * blk * 0.3321928f);   /* (dB/s x TLR, energy) */
        if (RVS.es > RVS.pk) RVS.pk = RVS.es;
        /* SWELL: the input's energy, fast (~5 ms) and slow (~30 ms); an onset pulls the factor down to 2 slow / fast */
        RVS.sf += (ei - RVS.sf) * (1.f - sy_exp2(-1.442695f * blk / 0.005f));
        RVS.ss += (ei - RVS.ss) * (1.f - sy_exp2(-1.442695f * blk / 0.03f));
        if (RVS.v[7]) {
            const float T = SP(SWT) * (float)RVS.v[7] * (float)RVS.v[7] * (1.f / 16129.f);
            if (RVS.sf > 2.f * RVS.ss && RVS.sf > 1e-12f) {
                const float d = 2.f * RVS.ss / RVS.sf;
                if (d < RVS.sw) RVS.sw = d;
            }
            RVS.sw += blk / T;
            if (RVS.sw > 1.f) RVS.sw = 1.f;
        } else RVS.sw = 1.f;
    }
}
#if RVS_WIDTH
/* WIDTH (1.18C47; the prototype's C1): the wet's side x ws, the middle kept (the mono sum as
 * before, to float rounding): ws = 1 - RVS_WCN W while new .. 1 + RVS_WCW W rung out, W = WIDTH / 127. Where between
 * is a glide: its place wp moves straight towards 1 (wide) while the input is silent and back towards 0 while it
 * sounds (the shimmer's own test, RVS_WIN), a whole way taking W GLIDE's time; the gain follows wp's smoothstep (its
 * ends flat: no corner when the glide starts or turns), and moves at most RVS_WSL a second (WIDTH turned, the shimmer
 * on or off: a glide back to 1 when it stops). Straight over each block from the last block's gain: no steps. After
 * the loop took the network's output, so the loop is 1.18C46's. The flash, once a block; WIDTH 0 (or the shimmer off)
 * at rest: nothing */
RVCOLD void rvs_c1(float *lh, float *rh)
{
    rvs_t *R;
    float W, o, t, d, a, da;
    const float blk = (float)RVS_BLK / (RVF_RATE * 0.5f), sl = RVS_WSL * blk;
    int i;
    if (!RVS_OK) return;
    R = &RVS;
    W = R->on ? (float)R->v[5] * (1.f / 127.f) : 0.f;
    if (W == 0.f && R->wd == 0.f) { R->wp = 0.f; return; }
    {   /* the glide: a whole way in RVS_WT0 x 2^(k / 5) s (wst: hyb_rv_shim); the input sounds while its 30 ms
         * energy is within RVS_WID of its own peak (falling RVS_WIF dB a second: 1 - x for e^-x, x < 0.003) and above
         * RVS_WIN of the output's */
        const float st = R->wst != 0.f ? R->wst : blk * (1.f / RVS_WT0) * 0.5f;   /* (k 5) */
        float p, wi = R->wi * (1.f - 0.23025851f * RVS_WIF * blk);
        if (R->ss > wi) wi = R->ss;
        R->wi = wi;
        p = R->wp + (R->ss > RVS_WID * wi && R->ss > RVS_WIN * R->es ? -st : st);
        R->wp = p = p < 0.f ? 0.f : p > 1.f ? 1.f : p;
        o = p * p * (3.f - 2.f * p);
    }
    t = W * ((RVS_WCN + RVS_WCW) * o - RVS_WCN);   /* (the side's gain - 1 wanted) */
    d = t - R->wd;
    d = d > sl ? sl : d < -sl ? -sl : d;
    a = 1.f + 0.5f * R->wd;             /* (l' = a l + (1 - a) r, r' = (1 - a) l + a r: the middle kept, the side x
                                         * 2 a - 1) */
    da = 0.5f * d * (1.f / (float)RVS_BLK);
    R->wd += d;
    for (i = 0; i < RVS_BLK; i++) {
        const float l = lh[i], r = rh[i];
        a += da;
        lh[i] = a * l + (1.f - a) * r; rh[i] = (1.f - a) * l + a * r;
    }
}
#endif
#endif
