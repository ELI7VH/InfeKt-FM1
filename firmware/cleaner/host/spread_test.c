/* SPDX-License-Identifier: GPL-3.0-only */
/* spread_test: the voice starts of a big chord spread over blocks (synth.c START_SPREAD), checked on the host engine.
 *   spread_test [-v]      (make spread: built plain and with HYB_MULTI + SY_UNI_CAP, as the firmware's engine)
 * Cases: 8, 12 and 16 keys struck in one millisecond with unison 1, 2 and 4 (the init patch, UNISON = n - 1); per
 * block the voices started (the age count's step) and per key the blocks its note-on waited (from its due block to
 * the block its voices started). Checks: no block starts more than the budget plus a key's unison less one (a key's
 * voices start together), unless its note-on had waited START_LATE_MAX blocks; no note-on waits more than
 * START_LATE_MAX blocks; every key starts, all of its unison voices in one block; the note-offs struck in the same
 * millisecond (a note released before its start) still start the note and release it, no voice left gated; MIDI
 * clock ticks behind a waiting note-on are taken in their own block; a single note and a chord within the budget
 * start in their due block. Exit 1 on a failed check. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "synth.h"
#include "../fm1/init_patch.h"

#ifndef START_LATE_MAX
#define START_LATE_MAX 4
#endif

static sy_synth S;
#ifdef HYB_MULTI
static sy_synth SB;
static sy_shared_t SH;
static float CH_A[2][2048], CH_B[2][2048], SAT_B[512 + 1];
static sy_multi_t M;
#define MIDI(st, d1, d2) sy_multi_midi(&M, st, d1, d2)
#define RENDER(l, r) sy_multi_render(&M, l, r, 0, 0)
#else
#define MIDI(st, d1, d2) sy_midi(&S, st, d1, d2)
#define RENDER(l, r) sy_render(&S, l, r, 0, 0)
#endif

static int verbose, fails;
static float L[SY_BLOCK], R[SY_BLOCK];
static int due[128], started[128], nstart[128];   /* per key: its due block, the block its note-on was taken, its voices
                                         * seen started (one block) */
static uint32_t evi[128];               /* per key: its note-on's place in the queue */
static int vblk[128];                   /* per key: the block its voices were seen starting */
static uint32_t seen_age;

static void fail(const char *what, int a, int b)
{
    printf("FAIL %s (%d %d)\n", what, a, b);
    fails++;
}

static void setup(int uni)
{
    uint8_t p[256];
    memcpy(p, INIT_PATCH, 256);
    p[97] = (uint8_t)(uni - 1);         /* UNISON */
    p[63] = 40;                         /* AE_R: a release that sounds on */
#ifdef HYB_MULTI
    memset(&M, 0, sizeof M);
    sy_multi_init(&M, &S, &SB, &SH, CH_A, CH_B, SAT_B, 46875.f);
    S.channel = 0;
    M.max_voices = 16;
    M.uni_max = 0;                      /* (no unison cap: the patch's count) */
    M.uni_steal = 0;
#else
    sy_init(&S, 46875.f);
    S.max_voices = SY_MAX_VOICES;
#endif
    sy_load_patch(&S, p);
    memset(due, -1, sizeof due);
    memset(started, -1, sizeof started);
    memset(nstart, 0, sizeof nstart);
    memset(vblk, -1, sizeof vblk);
    RENDER(L, R);
    RENDER(L, R);
    seen_age = S.ages;
}

static void on(int k)
{
    MIDI(0x90, (uint8_t)k, 100);
    due[k] = (int)S.evq[(S.ev_w - 1u) % SY_EVQ].at;
    evi[k] = S.ev_w - 1u;
    started[k] = vblk[k] = -1;
    nstart[k] = 0;
}

/* a block: the voices started in it (the ages since the last), each one's key */
static int block(void)
{
    int i, n;
    const uint32_t a = S.ages, b = S.block;
    RENDER(L, R);
    n = (int)(S.ages - a);
    for (i = 0; i < 128; i++)
        if (due[i] >= 0 && started[i] < 0 && (int32_t)(S.ev_r - evi[i]) > 0) started[i] = (int)b;
    for (i = 0; i < SY_MAX_VOICES; i++) {
        const sy_voice_t *v = &S.v[i];
        if (v->age > seen_age && v->age <= S.ages) {
            if (vblk[v->key] >= 0 && vblk[v->key] != (int)b) fail("a key's unison voices in two blocks", v->key, (int)b);
            if (started[v->key] != (int)b) fail("a key's voices not in its note-on's block", v->key, (int)b);
            vblk[v->key] = (int)b;
            nstart[v->key]++;
        }
    }
    seen_age = S.ages;
    return n;
}

static int gated(void)
{
    int i, g = 0;
    for (i = 0; i < SY_MAX_VOICES; i++) g += S.v[i].on && S.v[i].gate;
    return g;
}

/* nk keys at once, unison uni; offs: their note-offs in the same millisecond. Returns the most a note-on waited */
static int chord(int nk, int uni, int offs, int budget, int *peak)
{
    static const int K[16] = {36, 40, 43, 47, 48, 52, 55, 59, 60, 64, 67, 71, 72, 76, 79, 83};
    int i, b, wmax = 0, pk = 0;
    setup(uni);
    for (i = 0; i < nk; i++) on(K[i]);
    if (offs) for (i = 0; i < nk; i++) MIDI(0x80, (uint8_t)K[i], 0);
    for (b = 0; b < 12; b++) {
        const int n = block();
        if (n > pk) pk = n;
        if (verbose) printf("  block %u: %d started, %d gated\n", (unsigned)S.block - 1u, n, gated());
    }
    for (i = 0; i < nk; i++) {
        const int k = K[i], w = started[k] - due[k];
        if (started[k] < 0) { fail("a key never started", k, nk); continue; }
        if (nstart[k] != uni && nk * uni <= SY_MAX_VOICES) fail("a key's unison voices", k, nstart[k]);
        if (w > wmax) wmax = w;
    }
    if (wmax > START_LATE_MAX) fail("a note-on waited too long", wmax, START_LATE_MAX);
    if (budget && pk > budget + uni - 1 && wmax < START_LATE_MAX) fail("a block over the budget", pk, budget);
    if (offs && gated()) fail("a voice gated after its note-off", gated(), nk);
    *peak = pk;
    return wmax;
}

int main(int argc, char **argv)
{
    static const int NK[3] = {8, 12, 16}, UNI[3] = {1, 2, 4};
    int a, u, pk, w, budget = sy_start_budget;
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    printf("budget %d voice starts a block, START_LATE_MAX %d\n", budget, START_LATE_MAX);
    for (a = 0; a < 3; a++)
        for (u = 0; u < 3; u++) {
            w = chord(NK[a], UNI[u], 0, budget, &pk);
            printf("%2d keys, unison %d: the most started in a block %2d, the longest wait %d blocks\n", NK[a], UNI[u], pk, w);
        }
    w = chord(16, 2, 1, budget, &pk);
    printf("16 keys, unison 2, released in the same ms: the most started in a block %2d, the longest wait %d blocks\n", pk, w);
    {   /* a single note and a chord of 4 x unison 2 (within the budget): no wait */
        w = chord(1, 4, 0, budget, &pk);
        if (w) fail("a single note waited", w, 0);
        w = chord(4, 2, 0, budget, &pk);
        if (budget >= 8 && w) fail("a chord within the budget waited", w, 0);
    }
    {   /* the arpeggiator on: its keys never wait (it starts its notes itself, after the events) */
        int i;
        uint32_t at;
        setup(2);
        sy_set_param(&S, 129, 1);       /* (B_ARP) */
        for (i = 0; i < 16; i++) on(40 + i);
        at = S.evq[(S.ev_w - 1u) % SY_EVQ].at;
        while (S.block <= at) block();
        if (S.ev_r != S.ev_w) fail("the arpeggiator's keys waited", (int)(S.ev_w - S.ev_r), 0);
        printf("arpeggiator on, 16 keys: all taken in their block\n");
    }
#ifdef HYB_MULTI
    {   /* LAYER: both parts' starts count together; part B not left waiting behind part A */
        int i, b, n, pk = 0, b0 = -1;
        uint32_t at, sa, sb;
        uint8_t p[256];
        setup(2);
        memcpy(p, S.patch, 256);
        sy_load_patch(&SB, p);
        M.mode = SY_M_LAYER;
        RENDER(L, R);
        for (i = 0; i < 8; i++) on(48 + i * 3);
        at = S.evq[(S.ev_w - 1u) % SY_EVQ].at;
        while (S.block < at) RENDER(L, R);
        for (b = 0; b < 8; b++) {
            sa = S.ages; sb = SB.ages;
            RENDER(L, R);
            n = (int)(S.ages - sa + SB.ages - sb);
            if (n > pk) pk = n;
            if (SB.ages != sb && b0 < 0) b0 = b;
            if ((int)(S.block - 1u - at) == START_LATE_MAX && (S.ev_r != S.ev_w || SB.ev_r != SB.ev_w))
                fail("LAYER: a note-on waited too long", (int)(S.ev_w - S.ev_r), (int)(SB.ev_w - SB.ev_r));
        }
        if (budget && pk > budget + 2) fail("LAYER: a block over the budget", pk, budget);
        if (budget && b0 != 0) fail("LAYER: part B's first start late", b0, 0);
        printf("LAYER, 8 keys x unison 2 (32 starts): the most started in a block %d, part B's first in block %d\n", pk, b0);
    }
#endif
    {   /* MIDI clock ticks behind waiting note-ons: taken in their block */
        int i, b;
        uint32_t t0, at;
        setup(4);
        t0 = S.xclk_ticks;
        for (i = 0; i < 16; i++) on(40 + i);
        MIDI(0xF8, 0, 0);
        at = S.evq[(S.ev_w - 1u) % SY_EVQ].at;
        while (S.block < at) {
            block();
            if (S.xclk_ticks != t0) fail("a tick taken before its block", (int)(S.xclk_ticks - t0), 0);
        }
        b = block();                    /* (the tick's block: the chord's first) */
        if (budget && b > budget + 3) fail("the clock's block over the budget", b, budget);
        if (S.xclk_ticks != t0 + 1u) fail("a tick behind waiting note-ons not taken in its block", (int)(S.xclk_ticks - t0), 1);
    }
    printf(fails ? "spread_test: %d FAILED\n" : "spread_test: ok\n", fails);
    return fails ? 1 : 0;
}
