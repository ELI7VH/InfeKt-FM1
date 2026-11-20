/* SPDX-License-Identifier: GPL-3.0-only */
/* fknob_test: GLO > AUDIO > FILT KNOBS (synth.c flt_glide, s->flt_sm) on the host engine.
 *   fknob_test [-v]       (make fknob: built plain and with HYB_MULTI + SY_UNI_CAP, as the firmware's engine)
 * Checks, a held note of the init patch: ORIGINAL steps (cutoff 1, 2, resonance 1, 2 take a new value at the next
 * block); SMOOTH glides each in a straight line (between the old and the new value, monotonic, at the target in the
 * blocks flt_glide gives: FS_NS for a single step after a pause, FS_NJ for a jump, the time since the last move during
 * a turn) and then stays there; the other bytes still step; a patch load mid-glide takes the new patch's values at
 * once (no glide, nothing gliding after); the patch's smooth mode (B 25 odd) keeps its own glide; the passband gain's
 * ramp flags (sy_voice_t pr) are set only while a value moves and on voices that sounded before, and cleared after;
 * a controller (CC 40) and sy_set_param_queued glide alike; two parts: part B's move leaves part A alone. Exit 1 on a
 * failed check. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "synth.h"
#include "../fm1/init_patch.h"
/* (synth.c's constants: the same -D for both when a grid is tried) */
#ifndef FS_NC
#define FS_NC 200
#endif
#ifndef FS_NS
#define FS_NS 48
#endif
#ifndef FS_NJ
#define FS_NJ 4
#endif
#define PAUSE() for (int k_ = 0; k_ < FS_NC + 10; k_++) RENDER()   /* (more than FS_NC blocks: a pause) */

static sy_synth S;
#ifdef HYB_MULTI
static sy_synth SB;
static sy_shared_t SH;
static float CH_A[2][2048], CH_B[2][2048], SAT_B[512 + 1];
static sy_multi_t M;
#define RENDER() sy_multi_render(&M, L, R, 0, 0)
#else
#define RENDER() sy_render(&S, L, R, 0, 0)
#endif
static float L[SY_BLOCK], R[SY_BLOCK];
static int verbose, fails;

static void check(int ok, const char *what, double a, double b)
{
    if (!ok) { printf("FAIL %s (%g %g)\n", what, a, b); fails++; }
    else if (verbose) printf("  ok %s (%g %g)\n", what, a, b);
}

static void setup(int smooth)
{
    uint8_t p[256];
    memcpy(p, INIT_PATCH, 256);
    p[40] = 60; p[41] = 64; p[42] = 40; p[43] = 20;   /* cutoff 1, 2, resonance 1, 2 */
#ifdef HYB_MULTI
    memset(&M, 0, sizeof M);
    sy_multi_init(&M, &S, &SB, &SH, CH_A, CH_B, SAT_B, 46875.f);
    S.channel = 0;
    sy_load_patch(&SB, p);
    SB.flt_sm = (uint8_t)smooth;
#else
    sy_init(&S, 46875.f);
#endif
    S.opt = 0;                          /* (no queue latency: a message is taken at the next block) */
    sy_load_patch(&S, p);
    S.flt_sm = (uint8_t)smooth;
    sy_midi(&S, 0x90, 48, 100);
    PAUSE();
}

static int pr_any(const sy_synth *s)
{
    int i, n = 0;
#ifdef HYB_MULTI
    for (i = 0; i < s->nv; i++) n += s->v[i].pr;
#else
    for (i = 0; i < SY_MAX_VOICES; i++) n += s->v[i].pr;
#endif
    return n;
}

/* a move of byte by to val; the blocks until pv reaches it (checking a straight, monotonic glide), -1 never */
static int glide_blocks(sy_synth *s, int by, int val, int max, int *prs)
{
    float x0 = s->pv[by], last = x0;
    int n;
    *prs = 0;
    sy_set_param(s, by, val);
    for (n = 1; n <= max; n++) {
        RENDER();
        *prs += pr_any(s) > 0;
        if ((val > x0 && s->pv[by] < last) || (val < x0 && s->pv[by] > last)) { check(0, "monotonic", last, s->pv[by]); return -1; }
        last = s->pv[by];
        if (s->pv[by] == (float)val) return n;
    }
    return -1;
}

/* -stair: slow turns of cutoff 1 (single steps up every `per` blocks, 12 of them, after a pause): the share of the
 * blocks from the first move to the last move's settling in which pv stands still (a plateau: the staircase), the mean
 * blocks from a move until pv is within 0.1 of it (the lag), and the longest plateau; then a single step after a pause */
static void stair(void)
{
    static const int PER[4] = {37, 60, 123, 200};
    int j;
    printf("FS_NC %d FS_NS %d FS_NJ %d (blocks of 128 at 46875 Hz: 2.73 ms)\n", FS_NC, FS_NS, FS_NJ);
    for (j = 0; j < 4; j++) {
        const int per = PER[j], nm = 12;
        int k, b, still = 0, tot = 0, run = 0, lmax = 0, lag = 0, nl = 0;
        float last;
        setup(1);
        last = S.pv[40];
        for (k = 0; k < nm; k++) {
            const int tgt = S.patch[40] + 1;
            int got = -1;
            sy_set_param(&S, 40, tgt);
            for (b = 0; b < (k == nm - 1 ? per + FS_NC : per); b++) {
                RENDER();
                if (got < 0 && S.pv[40] >= (float)tgt - 0.1f) got = b + 1;
                if (k == nm - 1 && S.pv[40] == (float)tgt && got >= 0 && b + 1 > got) break;
                tot++;
                if (S.pv[40] == last) { still++; if (++run > lmax) lmax = run; } else run = 0;
                last = S.pv[40];
            }
            if (got > 0) { lag += got; nl++; }
        }
        printf("  a step every %3d blocks (%4.0f ms): plateau %5.1f %% of the time, the longest %3d blocks (%4.0f ms); "
               "lag %5.1f blocks (%4.0f ms)\n", per, per * 2.7307, 100. * still / tot, lmax, lmax * 2.7307,
               nl ? (double)lag / nl : -1., nl ? 2.7307 * lag / nl : -1.);
    }
}

int main(int argc, char **argv)
{
    int n, prs, i;
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    if (argc > 1 && !strcmp(argv[1], "-stair")) { stair(); return 0; }
    /* ORIGINAL: a step at the next block, every one of the four */
    setup(0);
    for (i = 40; i < 44; i++) {
        n = glide_blocks(&S, i, S.patch[i] + 7, 50, &prs);
        check(n == 1, "ORIGINAL steps at once", i, n);
        check(prs == 0, "ORIGINAL no ramp flags", i, prs);
    }
    /* SMOOTH: a single step after a pause over FS_NS blocks, a jump over FS_NJ (a pause: over FS_NC blocks) */
    setup(1);
    for (i = 40; i < 44; i++) {
        float x0 = S.pv[i];
        n = glide_blocks(&S, i, S.patch[i] + 1, FS_NS + 10, &prs);
        check(n == FS_NS || n == FS_NS + 1, "SMOOTH single step after a pause: FS_NS blocks (+1: the float steps)", i, n);
        check(prs == n, "SMOOTH ramp flags while it moves", i, prs);
        RENDER(); RENDER();
        check(pr_any(&S) == 0 && !S.fs_act, "SMOOTH flags cleared after", i, pr_any(&S));
        PAUSE();
        n = glide_blocks(&S, i, (int)x0 + 21, 100, &prs);
        check(n == FS_NJ, "SMOOTH jump after a pause: FS_NJ blocks", i, n);
        PAUSE();
    }
    /* a turn: moves every 10 blocks glide over 10 blocks each, one line */
    {
        int v = S.patch[40], k, b;
        float prev = S.pv[40];
        for (k = 0; k < 5; k++) {
            sy_set_param(&S, 40, v += 3);
            for (b = 0; b < 10; b++) {
                RENDER();
                if (k > 0) check(S.pv[40] > prev + 0.25f && S.pv[40] < prev + 0.35f, "SMOOTH turn: 0.3 a block", prev, S.pv[40]);
                prev = S.pv[40];
            }
        }
        check(S.pv[40] == (float)v, "SMOOTH turn ends on the target", S.pv[40], v);
    }
    /* other bytes step at once in SMOOTH */
    sy_set_param(&S, 44, 90);
    RENDER();
    check(S.pv[44] == 90.f, "SMOOTH: another byte steps", S.pv[44], 90);
    /* a controller and the queue glide alike */
    PAUSE();
    sy_midi(&S, 0xB0, 40, (uint8_t)(S.patch[40] + 1));
    RENDER(); RENDER();
    check(S.pv[40] != (float)S.patch[40] && S.fs_act, "SMOOTH: CC 40 glides", S.pv[40], S.patch[40]);
    PAUSE();
    sy_set_param_queued(&S, 42, S.patch[42] + 1);
    RENDER(); RENDER();
    check(S.pv[42] != (float)S.patch[42], "SMOOTH: a queued byte glides", S.pv[42], S.patch[42]);
    /* a patch load mid-glide: the new patch's values at once, nothing gliding */
    sy_set_param(&S, 40, 20);
    RENDER(); RENDER();
    {
        uint8_t p[256];
        memcpy(p, INIT_PATCH, 256);
        p[40] = 100; p[42] = 70;
        sy_load_patch(&S, p);
        sy_midi(&S, 0x90, 50, 100);
        RENDER();
        check(S.pv[40] == 100.f && S.pv[42] == 70.f, "patch load: no glide", S.pv[40], S.pv[42]);
        RENDER();
        check(!S.fs_act && pr_any(&S) == 0, "patch load: nothing gliding", S.fs_act, pr_any(&S));
    }
    /* the patch's smooth mode: its own 42 ms glide (smooth_step), also the first move after the load */
    {
        uint8_t p[256];
        memcpy(p, INIT_PATCH, 256);
        p[40] = 60; p[128 + 25] = 1;
        sy_load_patch(&S, p);
        RENDER();
        n = glide_blocks(&S, 40, 61, 100, &prs);
        check(n == 16, "smooth mode: its own glide (42 ms), the first move too", n, 16);   /* (0.042 s / 128 frames) */
        check(prs == 0, "smooth mode: no ramp flags", prs, 0);
    }
#ifdef HYB_MULTI
    /* two parts: B's move glides in B, A stays */
    setup(1);
    sy_load_patch(&SB, INIT_PATCH);
    SB.flt_sm = 1;
    for (int k = 0; k < 40; k++) RENDER();
    {
        float a0 = S.pv[40];
        sy_set_param(&SB, 40, SB.patch[40] - 30);
        RENDER(); RENDER();
        check(SB.pv[40] != (float)SB.patch[40] && SB.fs_act, "part B glides", SB.pv[40], SB.patch[40]);
        check(S.pv[40] == a0 && !S.fs_act, "part A untouched", S.pv[40], a0);
    }
#endif
    printf("%s: %d failed\n", argv[0], fails);
    return fails ? 1 : 0;
}
