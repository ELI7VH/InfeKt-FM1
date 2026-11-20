/* SPDX-License-Identifier: GPL-3.0-only */
/* bud_test: the budget's engine side (1.18C49: synth.c sy_bud; the firmware's early fades, app.c GOV_BUD,
 * are sy_shed calls with BUD_FADE's 15 blocks) on the host engine as the firmware builds it (HYB_MULTI + SY_UNI_CAP,
 * SINGLE), with an invented heavy patch: the init patch, UNISON 4, both oscillators, the filter half closed, a slow
 * attack and a long release (67), chorus and a reverb send.
 *   bud_test [-v]      (make bud)
 * Cases (the limit 10, as the governor sets it for a patch over the budget; UNI MAX 4):
 *  1. sy_bud 0 (as 1.18C48): two-key chords re-struck every 0.5 s: the voices on run over the limit at the chord
 *     changes (the fades' UNI_OVER and the waiting voices' free ones), as before;
 *  2. sy_bud 1: the same: after the first chord never more voices on (those fading counted) than the limit; every
 *     note sounds within 8 blocks of its key; the high band (3rd difference) at the chord changes no more than 3 dB
 *     over 1.18C48's largest step (no click: the waiting voices start as the fades end);
 *  3. sy_bud 1, eight keys rolled in and held (the pool at 14, the limit then 10): early fades one at a time (sy_shed, 15 blocks,
 *     8 apart) while the voices not fading exceed the limit: every key keeps a voice, no voice over the limit is
 *     left, the high band during the fades no more than 6 dB over the same blocks without them (a click
 *     would be 20 dB and more);
 *  4. the same as 3 with sy_bud 0 and no early fade: the output differs from 3 only from the first fade on
 *     (nothing acts before).
 * Exit 1 on a failed check. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "synth.h"
#include "../fm1/init_patch.h"

static sy_synth S, SB;
static sy_shared_t SH;
static float CH_A[2][2048], CH_B[2][2048], SAT_B[512 + 1];
static sy_multi_t M;
static float L[SY_BLOCK], R[SY_BLOCK];
static int verbose, fails;
static float worst;                     /* (chords: the largest step at a change, dB over the median) */
#define NB 2400
static float hf[NB];                    /* the high band per block: the largest 3rd difference of the left output */
static float out3[NB * SY_BLOCK];
static int nblk;
static float d1, d2, d3;                /* (the previous samples, for the 3rd difference) */

static void fail(const char *what, double a, double b)
{
    printf("FAIL %s (%g %g)\n", what, a, b);
    fails++;
}

static int on_n(int fading)
{
    int i, c = 0;
    for (i = 0; i < S.nv; i++) c += S.v[i].on && (fading || !S.v[i].ufade);
    return c;
}

static int gated(int key)
{
    int i, c = 0;
    for (i = 0; i < S.nv; i++) c += S.v[i].on && S.v[i].gate && S.v[i].key == key;
    return c;
}

static void block(void)
{
    int k;
    float m = 0.f;
    sy_multi_render(&M, L, R, 0, 0);
    for (k = 0; k < SY_BLOCK; k++) {
        const float d = L[k] - 3.f * d1 + 3.f * d2 - d3;
        d3 = d2; d2 = d1; d1 = L[k];
        if (fabsf(d) > m) m = fabsf(d);
        if (nblk < NB) out3[nblk * SY_BLOCK + k] = L[k];
    }
    if (nblk < NB) hf[nblk] = m;
    nblk++;
}

static void setup(int bud)
{
    uint8_t p[256];
    memcpy(p, INIT_PATCH, 256);
    p[97] = 3;                          /* UNISON: 4 voices a note */
    p[98] = 100; p[99] = 127;           /* its detune, spread */
    p[33] = 64; p[26] = 70;             /* both oscillators, osc 2 detuned */
    p[40] = 64; p[42] = 30;             /* the filter half closed, a little resonance */
    p[59] = 20; p[60] = 90; p[61] = 100; p[63] = 67;   /* a slow attack, a long release */
    p[105] = 60;                        /* chorus */
    p[112] = 2; p[113] = 60; p[116] = 90;   /* a reverb send */
    memset(&M, 0, sizeof M);
    sy_multi_init(&M, &S, &SB, &SH, CH_A, CH_B, SAT_B, 46875.f);
    S.channel = 0;
    M.max_voices = 16;
    M.uni_max = 4;
    M.uni_steal = 1;                    /* (UNISON FIRST, as shipped) */
    sy_load_patch(&S, p);
    M.max_voices = 16;
    sy_bud = 0;
    nblk = 0; d1 = d2 = d3 = 0.f;
    for (int i = 0; i < 2; i++) block();
    sy_bud = (uint8_t)bud;
}

static float db(float x) { return 20.f * log10f(x + 1e-12f); }

static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

static float median(int a, int b)       /* of hf[a..b) */
{
    static float t[NB];
    int n = 0, i;
    for (i = a; i < b; i++) t[n++] = hf[i];
    qsort(t, (size_t)n, sizeof t[0], cmpf);
    return t[n / 2];
}

/* two-key chords every 183 blocks (0.5 s), each released at the next; returns the most voices on after the first */
static int chords(int lim, int check)
{
    static const int C[4][2] = {{57, 64}, {55, 62}, {53, 60}, {52, 59}};
    int c, b, most = 0;
    worst = -99.f;
    for (c = 0; c < 8; c++) {
        const int at = nblk;
        if (c) { sy_multi_midi(&M, 0x80, C[(c - 1) & 3][0], 0); sy_multi_midi(&M, 0x80, C[(c - 1) & 3][1], 0); }
        sy_multi_midi(&M, 0x90, C[c & 3][0], 100);
        sy_multi_midi(&M, 0x90, C[c & 3][1], 100);
        if (c == 0) M.max_voices = lim; /* (the limit from the first chord on, as the governor learns it) */
        for (b = 0; b < 183; b++) {
            block();
            if (c && on_n(1) > most) most = on_n(1);
            if (check && c && b == 8 && (!gated(C[c & 3][0]) || !gated(C[c & 3][1])))
                fail("a note sounds within 8 blocks", gated(C[c & 3][0]), gated(C[c & 3][1]));
        }
        if (c) {
            const float st = median(at + 40, at + 170);
            float pk = 0.f;
            for (b = at - 1; b < at + 12; b++) if (hf[b] > pk) pk = hf[b];
            if (verbose) printf("  chord %d: high band at the change %+.1f dB over the steady median\n", c, db(pk) - db(st));
            if (db(pk) - db(st) > worst) worst = db(pk) - db(st);
        }
    }
    return most;
}

static const int K8[8] = {48, 52, 55, 59, 60, 64, 67, 71};

static int held8(int bud, int trim)     /* eight keys; the limit 10 after 100 blocks; returns the first fade's block */
{
    int k, b, first = -1, last = 0, n = 0;
    setup(0);
    M.max_voices = 14;
    for (k = 0; k < 8; k++) {           /* (the keys rolled, a block apart: a chord's keys in one block drop whole notes
                                         * in 1.18C48 already) */
        sy_multi_midi(&M, 0x90, K8[k], 100);
        block();
    }
    for (b = 8; b < 100; b++) block();
    if (verbose) { printf("  eight keys, voices a key:"); for (k = 0; k < 8; k++) printf(" %d", gated(K8[k])); printf("\n"); }
    M.max_voices = 10;
    sy_bud = (uint8_t)bud;
    for (b = 0; b < 700; b++) {
        if (trim && b - last >= 8 && on_n(0) > S.max_voices && sy_shed(&S, 1, 15)) {
            last = b;
            n++;
            if (first < 0) first = nblk;
        }
        block();
    }
    if (verbose && trim) {
        printf("  eight keys: %d faded early, %d on, %d fading; voices a key:", n, on_n(1), on_n(1) - on_n(0));
        for (k = 0; k < 8; k++) printf(" %d", gated(K8[k]));
        printf("\n");
    }
    return first;
}

int main(int argc, char **argv)
{
    int most, k, f, b;
    float w0;
    pid_t pid;
    static float ref[NB * SY_BLOCK], href[NB];
    verbose = argc > 1 && !strcmp(argv[1], "-v");

    /* 1. as 1.18C48 */
    setup(0);
    most = chords(10, 0);
    if (verbose) printf("1. sy_bud 0: at most %d voices on after the first chord (the limit 10)\n", most);
    if (most <= 10) fail("sy_bud 0: the voices over the limit at the changes, as before", most, 10);

    w0 = worst;
    /* 2. the budget's engine side */
    setup(1);
    most = chords(10, 1);
    if (verbose) printf("2. sy_bud 1: at most %d voices on after the first chord; the largest step %+.1f dB (as 1.18C48: %+.1f)\n", most, worst, w0);
    if (most > 10) fail("sy_bud 1: never over the limit", most, 10);
    if (worst > w0 + 3.f) fail("chord changes: the high band's step no more than 3 dB over 1.18C48's", worst, w0);

    /* 3. early fades of eight held keys' unison voices; 4. nothing acts before the first (both runs from the same
     * state: a child runs the one without fades) */
    fflush(stdout);
    if ((pid = fork()) == 0) {
        FILE *o = fopen("build/bud_test.ref", "wb");
        verbose = 0;
        held8(0, 0);
        fwrite(out3, sizeof out3[0], NB * SY_BLOCK, o);
        fwrite(hf, sizeof hf[0], NB, o);
        fclose(o);
        _exit(0);
    }
    waitpid(pid, 0, 0);
    {
        FILE *o = fopen("build/bud_test.ref", "rb");
        if (!o || fread(ref, sizeof ref[0], NB * SY_BLOCK, o) != NB * SY_BLOCK || fread(href, sizeof href[0], NB, o) != NB)
            fail("the reference run", 0, 0);
        if (o) fclose(o);
        remove("build/bud_test.ref");
    }
    f = held8(1, 1);
    if (f < 0) fail("eight keys: an early fade", f, 0);
    if (on_n(1) > 10) fail("eight keys: the voices within the limit", on_n(1), 10);
    for (k = 0; k < 8; k++) if (!gated(K8[k])) fail("eight keys: every key keeps a voice", K8[k], 0);
    {
        float up = -99.f, dn = 99.f;
        for (b = f; b < nblk && b < NB; b++) {
            const float d = db(hf[b]) - db(href[b]);
            if (d > up) up = d;
            if (d < dn) dn = d;
        }
        if (verbose) printf("3. the high band during the early fades %+.1f .. %+.1f dB against the same blocks without them\n", dn, up);
        if (up > 6.f) fail("early fades: the high band over the run without them + 6 dB (a click: 20 dB and more)", up, 6);
    }
    for (k = 0; k < f * SY_BLOCK; k++)
        if (ref[k] != out3[k]) { fail("before the first early fade: the output to the bit", k, f * SY_BLOCK); break; }
    if (verbose) printf("4. bit-identical up to the first early fade (block %d)\n", f);

    printf("%s bud_test\n", fails ? "FAIL" : "  ok");
    return fails ? 1 : 0;
}
