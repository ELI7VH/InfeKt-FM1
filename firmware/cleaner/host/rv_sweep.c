/* SPDX-License-Identifier: GPL-3.0-only */
/* rv_sweep: the reverb, the shimmer and the delay over their parameter space, for a sanitizer build (make sweep:
 * ASan + UBSan, the single build and the two-part builds with the 16-bit ring and with the lent float ring). From the
 * init patch with the reverb on, every value 0..127 of each effect byte (A 105-119), of the tempo (B 16) and of the
 * delay clock (B 20) in turn, then every value of each FX > SHIMMER control, with notes sounding and the state kept
 * between the cases (a change while it rings); REVERB HALF and FULL, lent and not. Fails on an output that is not
 * finite or beyond +-64. The first case is the combination of the bank patch that restarted 1.18C3 on the device (a
 * second core's stack overflow, not a parameter: hyb_rv_run's frame; fm1/stack_check.py guards that): mode 2, send
 * 0, pre-delay 63, decay 38, room 12, colour 64, the delay clock's 1/8 at 140 BPM (the bytes synthesised here). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "synth.h"
#include "../fm1/init_patch.h"
#ifndef PB
#define PB 128                          /* page B (sy_int.h) */
#endif

#ifdef HYB_RV_SWITCH
void hyb_rv_set_full(int full);
#endif
#ifdef HYB_SHIMMER
void hyb_rv_shim(const uint8_t *v);
#endif
static sy_synth S;
#ifdef HYB_MULTI
static sy_synth SB;
#ifdef HYB_RV_LEND
void hyb_rv_lend(float *ring);
#ifndef HYB_RV_LEND_PRE
#define HYB_RV_LEND_PRE 0u
#endif
static struct { float pre[HYB_RV_LEND_PRE / 4u + 1u]; float lo[16384]; sy_shared_t sh; } SHL;   /* (as lab_engine.c) */
#define SH SHL.sh
#else
static sy_shared_t SH;
#endif
static float CH_A[2][2048], CH_B[2][2048], SAT_B[512 + 1];
static sy_multi_t M;
#define MIDI(st, d1, d2) sy_multi_midi(&M, st, d1, d2)
#define RENDER(l, r, il, ir) sy_multi_render(&M, l, r, il, ir)
#else
#define MIDI(st, d1, d2) sy_midi(&S, st, d1, d2)
#define RENDER(l, r, il, ir) sy_render(&S, l, r, il, ir)
#endif

static long blocks, bad;

static void run(int n, const char *what, int k, int v)
{
    float l[SY_BLOCK], r[SY_BLOCK], il[SY_BLOCK] = {0}, ir[SY_BLOCK] = {0};
    int b, i;
    for (b = 0; b < n; b++) {
        RENDER(l, r, il, ir);
        blocks++;
        for (i = 0; i < SY_BLOCK; i++)
            if (!isfinite(l[i]) || !isfinite(r[i]) || fabsf(l[i]) > 64.f || fabsf(r[i]) > 64.f) {
                if (bad++ < 20) fprintf(stderr, "FAIL %s %d = %d: block %d, %g %g\n", what, k, v, b, l[i], r[i]);
                break;
            }
    }
}

static void notes(int on)
{
    static const uint8_t N[3] = {48, 60, 67};
    int i;
    for (i = 0; i < 3; i++) MIDI(on ? 0x90 : 0x80, N[i], on ? 110 : 0);
}

int main(int argc, char **argv)
{
    uint8_t p[256];
    int full, lend, k, v, nb = argc > 1 ? atoi(argv[1]) : 24;
    static const int B[] = {105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, PB + 16, PB + 20};
#ifdef HYB_MULTI
    sy_multi_init(&M, &S, &SB, &SH, CH_A, CH_B, SAT_B, 46875.f);
    S.channel = 0;
#else
    sy_init(&S, 46875.f);
#endif
    for (lend = 0; lend < 2; lend++) {
#ifdef HYB_RV_LEND
        hyb_rv_lend(lend ? SHL.lo : 0);
#else
        if (lend) break;
#endif
        for (full = 0; full < 2; full++) {
#ifdef HYB_RV_SWITCH
            hyb_rv_set_full(full);
#else
            if (full) break;
#endif
            /* the restarting patch's combination */
            memcpy(p, INIT_PATCH, 256);
            p[112] = 2; p[113] = 0; p[114] = 63; p[115] = 0; p[116] = 38; p[117] = 12; p[118] = 1; p[119] = 64;
            p[PB + 16] = 140 - 63; p[PB + 20] = 4;
            sy_load_patch(&S, p);
            notes(1); run(4 * nb, "a009", 0, 0); notes(0); run(nb, "a009", 0, 0);
            p[113] = 100; sy_load_patch(&S, p);
            notes(1); run(4 * nb, "a009-send", 0, 0); notes(0); run(nb, "a009-send", 0, 0);
            /* each byte over 0..127, the others at a reverb with signal */
            for (k = 0; k < (int)(sizeof B / sizeof B[0]); k++) {
                memcpy(p, INIT_PATCH, 256);
                p[112] = 2 + (k & 1) + (k & 2) / 2;   /* (modes 2, 3, 4, 4 in turn) */
                p[113] = 100; p[116] = 100; p[115] = 90; p[PB + 16] = 140 - 63; p[PB + 20] = (uint8_t)(k & 4 ? 4 : 0);
                notes(1);
                for (v = 0; v < 128; v++) {
                    p[B[k]] = (uint8_t)v;
                    sy_load_patch(&S, p);
                    run(nb, "byte", B[k], v);
                }
                notes(0); run(nb, "byte", B[k], 128);
            }
#ifdef HYB_SHIMMER
            for (k = 0; k < HYB_SHIM_N + 3; k++) {   /* (1.18C45: + LAYERS, GRAIN, MODULATION again with LFO 3 on SHIM
                                         * MOD, fast S&H; the others with MODULATION 127) */
                uint8_t sv[HYB_SHIM_N] = {100, 2, 64, 64, 127, 0, 0, 0};
                const int l3 = k >= HYB_SHIM_N;
                memcpy(p, INIT_PATCH, 256);
                p[112] = 2; p[113] = 100; p[116] = 110; p[PB + 16] = 140 - 63; p[PB + 20] = 4;
                if (l3) { p[PB + 7] = 127; p[PB + 8] = 4; p[PB + 12] = 127; p[PB + 11] = 3; }
                sy_load_patch(&S, p);
                S.shx_rt[11] = (uint8_t)(l3 ? 2 : 0);
                if (l3) k = k == HYB_SHIM_N ? 1 : k == HYB_SHIM_N + 1 ? 2 : 4;
                notes(1);
                for (v = 0; v < 128; v++) {
                    sv[k] = (uint8_t)v;
                    hyb_rv_shim(sv);
                    run(nb, "shimmer", k, v);
                }
                notes(0); run(4 * nb, "shimmer", k, 128);
                sv[0] = 0; hyb_rv_shim(sv);
                if (l3) k = k == 1 ? HYB_SHIM_N : k == 2 ? HYB_SHIM_N + 1 : HYB_SHIM_N + 2;
                S.shx_rt[11] = 0;
            }
            {   /* (1.18C47) WIDTH 127 with W GLIDE 0 and 15 (TAIL's byte, bits 1-4: k ^ 5): notes on and off (the glide
                 * both ways), WIDTH swept down while it rings, then AMOUNT off while wide (the side's gain glides back) */
                static const uint8_t G[2] = {0, 15};
                int g;
                for (g = 0; g < 2; g++) {
                    uint8_t sv[HYB_SHIM_N] = {100, 0, 64, 64, 0, 127, 0, 57};
                    sv[6] = (uint8_t)(((G[g] ^ 5u) & 15u) << 1);
                    memcpy(p, INIT_PATCH, 256);
                    p[112] = 2; p[113] = 100; p[116] = 110; p[117] = 3;
                    sy_load_patch(&S, p);
                    hyb_rv_shim(sv);
                    for (v = 0; v < 4; v++) { notes(1); run(2 * nb, "width", G[g], v); notes(0); run(6 * nb, "width", G[g], v); }
                    notes(1); run(nb, "width", G[g], 127);
                    for (v = 127; v >= 0; v--) { sv[5] = (uint8_t)v; hyb_rv_shim(sv); run(2, "width", G[g], v); }
                    sv[5] = 127; hyb_rv_shim(sv); run(2 * nb, "width", G[g], 127); notes(0); run(8 * nb, "width", G[g], 127);
                    sv[0] = 0; hyb_rv_shim(sv); run(8 * nb, "width-off", G[g], 0);
                }
            }
#endif
            fprintf(stderr, "lend %d full %d: %ld blocks, %ld bad\n", lend, full, blocks, bad);
        }
    }
    printf("%s: %ld blocks, %s\n", argv[0], blocks, bad ? "FAIL" : "ok");
    return bad != 0;
}
