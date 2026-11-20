/* SPDX-License-Identifier: GPL-3.0-only */
/* The cleaner's own reverb (modes 2-4): a network of this project's design whose numbers are measured
 * on the development engine's reverb as a black box (renders only: cleaner/tools/measure/reverb/, PROVENANCE.md). The interface is
 * the one the engine and the app call (the names kept: hyb_rv_*). */
#ifndef SY_CLN_RV_H
#define SY_CLN_RV_H
#include <stdint.h>

/* the ring's word: 32 a float; the two-part builds (HYB_MULTI) 16, a signed fixed-point word (sy_cln_rv.c q16;
 * HYB_RV_F16: the earlier 16-bit float, sign, 4-bit exponent, 11-bit mantissa, for comparisons), the ring 64 KB */
#ifndef HYB_RV_BITS
#ifdef HYB_MULTI
#define HYB_RV_BITS 16
#else
#define HYB_RV_BITS 32
#endif
#endif
#if !defined(HYB_RV_SWITCH) && !defined(HYB_RV_NOSWITCH)   /* GLO > AUDIO > REVERB: FULL / HALF */
#define HYB_RV_SWITCH 1
#endif
/* FX > SHIMMER: on REVERB HALF, its buffer the ring's second 64 KB (a float ring of 128 KB), or the lent ring's lower
 * half (HYB_RV_LEND: Bluetooth off at power-on; its state the HYB_RV_LEND_PRE bytes below the lent ring) */
#if !defined(HYB_SHIMMER) && !defined(HYB_NO_SHIMMER) && defined(HYB_RV_SWITCH) && (HYB_RV_BITS == 32 || defined(HYB_RV_LEND))
#define HYB_SHIMMER 1
#endif
#if defined(HYB_SHIMMER) && HYB_RV_BITS != 32
#ifndef HYB_RV_LEND
#error "HYB_SHIMMER on a 16-bit ring: on the lent memory only (HYB_RV_LEND)"
#endif
#define HYB_SHIM_LEND 1                 /* (the shimmer only while Bluetooth's memory is lent: the app's bt_lent) */
#define HYB_RV_LEND_PRE 1536u           /* the shimmer's state: the bytes right below the lent ring (lend_check.py) */
#endif
#ifdef HYB_SHIMMER
#define HYB_SHIM_N 8                    /* its controls: AMOUNT, LAYERS, GRAIN, TONE; 4 MODULATION (1.18C45), 5 WIDTH
                                         * (1.18C47; DIRECT before, always 0); 6 TAIL (bit 0: 0 LIMITED, 1 FREE; bits
                                         * 1-4 W GLIDE ^ 5, 1.18C47: 0 its default), 7 SWELL */
#endif

int hyb_rv_run(float *ring, const uint8_t *patch, int mode, float *l, float *r, float dry, float wet, float gin);
/* (1.18C39) the float ring's rest (sy_cln_rv.c RVF_GATE): while the reverb's input is silent it
 * asks for the part's output peak of the block (after the phaser, the EQ and the boost: sy_fx_render's end) */
#ifndef RVF_GATE
#define RVF_GATE 1
#endif
#if RVF_GATE
extern uint8_t hyb_rv_arm;              /* 1: sy_fx_render puts the block's output peak into hyb_rv_opk */
extern float hyb_rv_opk;
void hyb_rv_post(const float *l, const float *r);
#endif
void hyb_rv_reset(void);
int hyb_rv_health(uint32_t *h);         /* (the debug log's look at it) */
#define HYB_RV_QUIET 1                  /* the effects at rest (sy_fx.c SY_FX_REST): */
void hyb_rv_quiet(float *ring, const uint8_t *patch, int mode);   /* a block of zeros skipped: what moves by itself */
int hyb_rv_still(float *ring, int tiny);   /* 1: ring and states at zero (tiny: or subnormal, then 0) */
#ifdef HYB_RV_SWITCH
void hyb_rv_set_full(int full);         /* 0 HALF, 1 FULL; taken between blocks, the ring cleared */
int hyb_rv_is_full(void);               /* the path running */
#endif
#ifdef HYB_SHIMMER
void hyb_rv_shim(const uint8_t *v);     /* FX > SHIMMER: HYB_SHIM_N values (AMOUNT 0 off; > 0 in a reverb mode runs
                                         * HALF whatever hyb_rv_set_full says), taken at the reverb's next block */
int hyb_rv_shim_on(void);               /* the shimmer running (a host test's look) */
int hyb_rv_shim_get(int k);             /* (a host test's look: 0 AMOUNT, 1 MODULATION (1.18C45; TONE before) as the
                                         * loop takes them, 2 AMOUNT, 3 LAYERS as given, 4 the loop gain x 1000, 5 TAIL,
                                         * 6 SWELL, 7 MODULATION's depth now x 100 (cents)) */
extern float hyb_rv_shm[3];             /* the matrix's offsets on AMOUNT and MODULATION (1.18C45; TONE before), LFO 3
                                         * on SHIM MOD (-1..1) (sy_fx.c, the engine's shm) */
extern uint8_t hyb_rv_shl3;             /* (1.18C45) LFO 3's destination SHIM MOD: it moves the pitch, the reverb's LFO not */
#endif
#ifdef HYB_RV_LEND
void hyb_rv_lend(float *ring);          /* REVERB FULL on this float ring of 128 KB (Bluetooth's memory lent), before
                                         * the first block; 0 none */
#endif
#ifdef CLN_RV_TUNE
extern float cln_rv_tune[256];          /* (a measurement build: the design constants, sy_cln_rv.c CP) */
#ifdef HYB_SHIMMER
extern float cln_rvs_tune[16];          /* (the shimmer's, SP) */
#endif
#endif
#endif
