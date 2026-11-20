/* SPDX-License-Identifier: GPL-3.0-only */
/* shed_test: the overload guard's shedding (synth.c sy_shed, SY_SHED_KEEP) and sy_note_voices, on the host engine as
 * the firmware builds it (HYB_MULTI + SY_UNI_CAP, SINGLE).
 *   shed_test [-v]      (make shed)
 * Cases (the init patch with UNISON 4, a cap of 2 voices a note as GLO > AUDIO > UNISON MAX 2, and a long decay so the
 * older note is the quieter, as a test patch's on the device):
 *  1. two keys held, two voices each; the guard sheds 2, then 2 more, then 2 more: each note keeps one voice (the old
 *     rule took both voices of the first note at once, the note cut while the second sounded);
 *  2. a released tail is shed before a held unison voice;
 *  3. three notes of 2, 2 and 1 voices: the shed takes from the notes with two, never the lone note's voice;
 *  4. sy_note_voices: the patch's 4 under no cap, 2 under UNISON MAX 2, 1 under MAX 1.
 * Exit 1 on a failed check. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "synth.h"
#include "../fm1/init_patch.h"

static sy_synth S, SB;
static sy_shared_t SH;
static float CH_A[2][2048], CH_B[2][2048], SAT_B[512 + 1];
static sy_multi_t M;
static float L[SY_BLOCK], R[SY_BLOCK];
static int verbose, fails;

static void fail(const char *what, int a, int b)
{
    printf("FAIL %s (%d %d)\n", what, a, b);
    fails++;
}

static void blocks(int n)
{
    while (n-- > 0) sy_multi_render(&M, L, R, 0, 0);
}

static void setup(int cap)
{
    uint8_t p[256];
    memcpy(p, INIT_PATCH, 256);
    p[97] = 3;                          /* UNISON: 4 voices a note */
    p[94] = 0;                          /* KEY_MODE poly */
    p[60] = 60;                         /* AE_D: a decay, */
    p[61] = 20;                         /* AE_S: to a low sustain (the older note the quieter) */
    p[63] = 60;                         /* AE_R: a release that sounds on */
    memset(&M, 0, sizeof M);
    sy_multi_init(&M, &S, &SB, &SH, CH_A, CH_B, SAT_B, 46875.f);
    S.channel = 0;
    M.max_voices = 16;
    M.uni_max = (uint8_t)cap;
    M.uni_steal = 0;
    sy_load_patch(&S, p);
    blocks(2);
}

static int live(int key)                /* a held note's voices sounding, not fading */
{
    int i, c = 0;
    for (i = 0; i < S.nv; i++) c += S.v[i].on && S.v[i].gate && !S.v[i].ufade && S.v[i].key == key;
    return c;
}

static int tails(void)
{
    int i, c = 0;
    for (i = 0; i < S.nv; i++) c += S.v[i].on && !S.v[i].gate && !S.v[i].ufade;
    return c;
}

static void show(const char *t)
{
    if (verbose) printf("%-28s 60:%d 64:%d 67:%d tails %d active %d\n", t, live(60), live(64), live(67), tails(),
                        sy_voices_active(&S));
}

int main(int argc, char **argv)
{
    int k;
    verbose = argc > 1 && !strcmp(argv[1], "-v");

    /* 1. two keys, two voices each */
    setup(2);
    sy_multi_midi(&M, 0x90, 60, 100);
    blocks(200);                        /* (~0.5 s: the first note in its decay) */
    sy_multi_midi(&M, 0x90, 64, 100);
    blocks(3);
    show("two keys");
    if (live(60) != 2 || live(64) != 2) fail("two keys: 2 + 2 voices", live(60), live(64));
    k = sy_shed(&S, 2, 2);
    blocks(4);
    show("shed 2");
    if (k != 2) fail("shed 2: taken", k, 2);
    if (live(60) != 1 || live(64) != 1) fail("shed 2: one voice each note", live(60), live(64));
    k = sy_shed(&S, 2, 2);
    blocks(4);
    show("shed 2 more");
    if (k != 0) fail("shed again: nothing left to take (each note one voice)", k, 0);
    if (live(60) != 1 || live(64) != 1) fail("shed again: both notes sound", live(60), live(64));

    /* 2. a released tail first */
    setup(2);
    sy_multi_midi(&M, 0x90, 60, 100);
    blocks(20);
    sy_multi_midi(&M, 0x80, 60, 0);
    blocks(2);
    sy_multi_midi(&M, 0x90, 64, 100);
    blocks(3);
    show("tail + key");
    if (tails() != 2 || live(64) != 2) fail("tail + key: 2 tails, 2 held", tails(), live(64));
    k = sy_shed(&S, 1, 2);
    blocks(1);
    show("shed 1");
    if (k != 1 || tails() != 1 || live(64) != 2) fail("shed 1: a tail, not the held note", tails(), live(64));

    /* 3. notes of 2, 2 and 1 voices */
    setup(2);
    sy_multi_midi(&M, 0x90, 60, 100);
    blocks(3);
    sy_multi_midi(&M, 0x90, 64, 100);
    blocks(3);
    M.uni_max = 1;                      /* (the third note one voice, as UNISON AUTO's chord step would) */
    blocks(1);
    sy_multi_midi(&M, 0x90, 67, 100);
    blocks(3);
    show("2 + 2 + 1");
    if (live(60) != 2 || live(64) != 2 || live(67) != 1) fail("2 + 2 + 1 voices", live(60) * 10 + live(64), live(67));
    k = sy_shed(&S, 3, 2);
    blocks(4);
    show("shed 3");
    if (k != 2) fail("shed 3: two taken (the unison voices)", k, 2);
    if (live(60) != 1 || live(64) != 1 || live(67) != 1) fail("shed 3: every note one voice", live(60) * 10 + live(64), live(67));

    /* 4. sy_note_voices */
    setup(0);
    blocks(1);
    if (sy_note_voices(&S) != 4) fail("note voices, no cap", sy_note_voices(&S), 4);
    M.uni_max = 2; blocks(1);
    if (sy_note_voices(&S) != 2) fail("note voices, UNISON MAX 2", sy_note_voices(&S), 2);
    M.uni_max = 1; blocks(1);
    if (sy_note_voices(&S) != 1) fail("note voices, UNISON MAX 1", sy_note_voices(&S), 1);

    printf("shed_test: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
