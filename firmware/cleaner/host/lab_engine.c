/* SPDX-License-Identifier: GPL-3.0-only */
/* The engine as a command-line renderer with the lab's interface (tools/lab/lab.py):
 *   engine OUT.wav preset FILE run N on NOTE VEL off NOTE midi STATUS D1 D2 ...
 * a stereo 24-bit WAV at 46,875 Hz; the audio input from the raw int32 file named by $INPUT (24-bit values, left and
 * right in turn). SY_FS overrides the rate, SY_OPT the option flags (synth.h), SY_OSC_CLEAN osc 1 and 2's
 * ORIGINAL (0) .. CLEAN (1), SY_FLT_SMOOTH=1 the filters' base values gliding (GLO > AUDIO > FILT KNOBS SMOOTH),
 * SY_START_BUDGET the voice starts a block takes (synth.c START_SPREAD; 0 no limit), SY_REST=1: "rest B" on stderr at the
 * end, the block from which part A's effects rest to the end (-1: not resting at the end; tools/perf/tailtest.py).
 * Built with HYB_MULTI (two parts, synth.h sy_multi_t): preset loads part A, the notes and MIDI go through the setup
 * (sy_multi_midi; SINGLE at the start: part A alone, as without HYB_MULTI), and more commands: presetb FILE (part B's
 * patch), mode N (0 SINGLE, 1 LAYER, 2 SPLIT, 3 CHANNELS), split KEY, chb CH (0..15, -1 none), nva N (part A's
 * voices), sendb V (-1 B's patch's send, 0..127), dyn 0/1 (DYNAMIC), maxv N (the voices in all), unimax / unimono N
 * (the unison caps, 0 none), uniauto 0/1, steal 0/1 (UNISON FIRST), voices (each part's voices on, to stderr), a / b STATUS D1 D2 (a message to
 * one part alone). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "synth.h"

static sy_synth S;
/* SY_CPU2 set: the two-core paths run on this core (a job taken and done at once), for tests of a build with SY_CORES 2 */
static int cpu2_now(void (*fn)(uint32_t), uint32_t arg) { fn(arg); return 1; }
static int cpu2_done(void) { return 0; }
/* the reverb's switches as the app sets them (a firmware engine's build): SY_RVQ=0 HALF, 1 FULL (GLO > AUDIO > REVERB);
 * SY_SHIM=a,b,..: FX > SHIMMER's values (AMOUNT, LAYERS, GRAIN, TONE, HIGH, DIRECT, TAIL, SWELL) */
#ifdef HYB_RV_SWITCH
void hyb_rv_set_full(int full);
#endif
#ifdef HYB_SHIMMER
void hyb_rv_shim(const uint8_t *v);
#ifdef CLN_RVS_LOG
void cln_rvs_log(float *o);             /* (SY_SHLOG=FILE: the shimmer loop's gain, governor, SWELL, TAIL's energy and peak
                                         * a block, as text) */
static FILE *shlog;
extern float cln_rvs_cap[SY_BLOCK / 2];
static FILE *shcap;                     /* (SY_SHCAP=FILE: the shifters' output a block, raw floats at half the rate) */
#endif
#endif
#ifdef HYB_MULTI
static sy_synth SB;
#ifdef HYB_RV_LEND
/* (the Bluetooth two-part build's REVERB FULL on Bluetooth's lent memory: a float ring of 128 KB, the 64 KB right below
 * SH.sp, SH's first member, as the firmware's; SY_NOLEND=1 in the environment: none, the 16-bit ring) */
void hyb_rv_lend(float *ring);
#ifndef HYB_RV_LEND_PRE
#define HYB_RV_LEND_PRE 0u
#endif
static struct { float pre[HYB_RV_LEND_PRE / 4u + 1u]; float lo[16384]; sy_shared_t sh; } SHL;   /* (pre: the bytes below
                                         * the lent ring, the radio's .bss in the firmware: the shimmer's state, 1.04) */
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

static void put24(FILE *f, float x)
{
    long v = (long)(x * 8388608.f + (x >= 0 ? 0.5f : -0.5f));
    if (v > 8388607) v = 8388607;
    if (v < -8388608) v = -8388608;
    fputc((int)(v & 255), f); fputc((int)((v >> 8) & 255), f); fputc((int)((v >> 16) & 255), f);
}
static void le32(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc(v >> 24, f); }
static void le16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

int main(int argc, char **argv)
{
    FILE *out, *in = NULL;
    const char *ip = getenv("INPUT"), *fsv = getenv("SY_FS"), *op = getenv("SY_OPT");
    float fs = fsv ? (float)atof(fsv) : 46875.f, l[SY_BLOCK], r[SY_BLOCK], il[SY_BLOCK], ir[SY_BLOCK];
    unsigned frames = 0;
    long rest_at = -1;                  /* (the block from which part A's effects rest to the end; -1 none) */
    int i;
    if (argc < 2) { fprintf(stderr, "usage: %s OUT.wav preset FILE run N on NOTE VEL off NOTE midi ST D1 D2 ...\n", argv[0]); return 2; }
#ifdef HYB_MULTI
    sy_multi_init(&M, &S, &SB, &SH, CH_A, CH_B, SAT_B, fs);
#ifdef HYB_RV_LEND
    if ((char *)(void *)SH.sp.rvx_ring != (char *)(void *)(SHL.lo + 16384)) { fprintf(stderr, "lend: SH.sp not right after\n"); return 2; }
    if (!getenv("SY_NOLEND")) hyb_rv_lend(SHL.lo);
#endif
    S.channel = 0;                      /* (as sy_init leaves it) */
    if (op) S.opt = SB.opt = (uint32_t)strtoul(op, 0, 0);
#else
    sy_init(&S, fs);
    if (op) S.opt = (uint32_t)strtoul(op, 0, 0);
#endif
    if (getenv("SY_OSC_CLEAN")) {       /* osc 1 and 2: 0 ORIGINAL .. 1 CLEAN (GLO > AUDIO > OSC) */
        S.osc_clean = (float)atof(getenv("SY_OSC_CLEAN"));
#ifdef HYB_MULTI
        SB.osc_clean = S.osc_clean;
#endif
    }
    if (getenv("SY_FLT_SMOOTH")) {      /* the filters' base values: 0 ORIGINAL, 1 SMOOTH (GLO > AUDIO > FILT KNOBS) */
        S.flt_sm = (uint8_t)atoi(getenv("SY_FLT_SMOOTH"));
#ifdef HYB_MULTI
        SB.flt_sm = S.flt_sm;
#endif
    }
    if (getenv("SY_CPU2")) sy_set_cpu2(cpu2_now, cpu2_done);
    if (getenv("SY_START_BUDGET")) sy_start_budget = (uint8_t)atoi(getenv("SY_START_BUDGET"));   /* (synth.c START_SPREAD:
                                         * the voice starts a block takes, 0 no limit) */
#ifdef CLN_RV_TUNE
    if (getenv("CLN_RV_TUNE")) {         /* (a measurement build: "k=v,k=v": the own reverb's design constants) */
        const char *p = getenv("CLN_RV_TUNE");
        while (*p) {
            int k = atoi(p);
            while (*p && *p != '=') p++;
            if (!*p) break;
            if (k >= 0 && k < 256) cln_rv_tune[k] = (float)atof(++p);
            while (*p && *p != ',') p++;
            if (*p) p++;
        }
    }
#ifdef HYB_SHIMMER
    if (getenv("CLN_RVS_TUNE")) {        /* (the same: the shimmer's, cln_rvs_tune) */
        const char *p = getenv("CLN_RVS_TUNE");
        while (*p) {
            int k = atoi(p);
            while (*p && *p != '=') p++;
            if (!*p) break;
            if (k >= 0 && k < 16) cln_rvs_tune[k] = (float)atof(++p);
            while (*p && *p != ',') p++;
            if (*p) p++;
        }
    }
#endif
#endif
#ifdef HYB_RV_SWITCH
    if (getenv("SY_RVQ")) hyb_rv_set_full(atoi(getenv("SY_RVQ")));
#endif
#ifdef HYB_SHIMMER
    if (getenv("SY_SHIM")) {
        uint8_t v[HYB_SHIM_N] = {0};
        const char *p = getenv("SY_SHIM");
        int k;
        for (k = 0; k < HYB_SHIM_N && *p; k++) {
            v[k] = (uint8_t)atoi(p);
            while (*p && *p != ',') p++;
            if (*p) p++;
        }
        hyb_rv_shim(v);
    }
    if (getenv("SY_SHRT")) {             /* (1.18C45) "i=t,i=t": the shimmer's targets (synth.h shx_rt: 0-8 the matrix's
                                         * slots, 9, 10 LFO 1's and 2's assigns, 11 LFO 3; t 1 AMOUNT, 2 MODULATION) */
        const char *p = getenv("SY_SHRT");
        while (*p) {
            const int k = atoi(p);
            while (*p && *p != '=') p++;
            if (!*p) break;
            if (k >= 0 && k < 12) S.shx_rt[k] = (uint8_t)atoi(++p);
            while (*p && *p != ',') p++;
            if (*p) p++;
        }
    }
#endif
#if defined(HYB_SHIMMER) && defined(CLN_RVS_LOG)
    if (getenv("SY_SHLOG")) shlog = fopen(getenv("SY_SHLOG"), "w");
    if (getenv("SY_SHCAP")) shcap = fopen(getenv("SY_SHCAP"), "wb");
#endif
    if (ip) in = fopen(ip, "rb");
    out = fopen(argv[1], "wb");
    if (!out) { perror(argv[1]); return 1; }
    fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, out);
    le32(out, 16); le16(out, 1); le16(out, 2); le32(out, (unsigned)fs); le32(out, (unsigned)fs * 6); le16(out, 6); le16(out, 24);
    fwrite("data\0\0\0\0", 1, 8, out);
    for (i = 2; i < argc; i++) {
        const char *c = argv[i];
        if (!strcmp(c, "preset") && i + 1 < argc) {
            uint8_t p[256] = {0};
            FILE *pf = fopen(argv[++i], "rb");
            if (!pf || fread(p, 1, 256, pf) != 256) { fprintf(stderr, "preset %s: unreadable\n", argv[i]); return 1; }
            fclose(pf);
            sy_load_patch(&S, p);
        } else if (!strcmp(c, "on") && i + 2 < argc) {
            MIDI(0x90, (uint8_t)atoi(argv[i + 1]), (uint8_t)atoi(argv[i + 2])); i += 2;
        } else if (!strcmp(c, "off") && i + 1 < argc) {
            MIDI(0x80, (uint8_t)atoi(argv[i + 1]), 0); i += 1;
        } else if (!strcmp(c, "midi") && i + 3 < argc) {
            MIDI((uint8_t)strtol(argv[i + 1], 0, 0), (uint8_t)atoi(argv[i + 2]), (uint8_t)atoi(argv[i + 3])); i += 3;
        } else if (!strcmp(c, "shed") && i + 2 < argc) {   /* (the overload guard's shedding: N voices over NB blocks,
                                         * part A's; a firmware's app.c rvq_tick) */
            sy_shed(&S, atoi(argv[i + 1]), atoi(argv[i + 2])); i += 2;
        } else if (!strcmp(c, "run") && i + 1 < argc) {
            int n = atoi(argv[++i]), b, k;
            for (b = 0; b < n; b++) {
                for (k = 0; k < SY_BLOCK; k++) {
                    int32_t w[2] = {0, 0};
                    if (in && fread(w, 4, 2, in) != 2) { fclose(in); in = NULL; w[0] = w[1] = 0; }
                    il[k] = (float)w[0] / 8388608.f;
                    ir[k] = (float)w[1] / 8388608.f;
                }
                RENDER(l, r, il, ir);
#if defined(HYB_SHIMMER) && defined(CLN_RVS_LOG)
                if (shlog) { float o[8]; cln_rvs_log(o); fprintf(shlog, "%g %g %g %g %g %g %g %g\n", o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7]); }
                if (shcap) fwrite(cln_rvs_cap, sizeof cln_rvs_cap, 1, shcap);
#endif
                for (k = 0; k < SY_BLOCK; k++) { put24(out, l[k]); put24(out, r[k]); }
                frames += SY_BLOCK;
                if (!S.fx_rest) rest_at = -1;   /* (SY_REST: the effects' rest, tools/perf/tailtest.py) */
                else if (rest_at < 0) rest_at = (long)(frames / SY_BLOCK) - 1;
            }
#ifdef HYB_MULTI
        } else if (!strcmp(c, "presetb") && i + 1 < argc) {
            uint8_t p[256] = {0};
            FILE *pf = fopen(argv[++i], "rb");
            if (!pf || fread(p, 1, 256, pf) != 256) { fprintf(stderr, "preset %s: unreadable\n", argv[i]); return 1; }
            fclose(pf);
            sy_load_patch(&SB, p);
        } else if (!strcmp(c, "mode") && i + 1 < argc) { M.mode = (uint8_t)atoi(argv[++i]);
        } else if (!strcmp(c, "split") && i + 1 < argc) { M.split = (uint8_t)atoi(argv[++i]);
        } else if (!strcmp(c, "chb") && i + 1 < argc) { M.ch_b = (int8_t)atoi(argv[++i]);
        } else if (!strcmp(c, "nva") && i + 1 < argc) { M.nv_a = (uint8_t)atoi(argv[++i]);
        } else if (!strcmp(c, "sendb") && i + 1 < argc) { M.send_b = (int16_t)atoi(argv[++i]);
        } else if (!strcmp(c, "dyn") && i + 1 < argc) { M.dyn = (uint8_t)atoi(argv[++i]);
        } else if (!strcmp(c, "maxv") && i + 1 < argc) { M.max_voices = (int16_t)atoi(argv[++i]);
        } else if (!strcmp(c, "unimax") && i + 1 < argc) { M.uni_max = (uint8_t)atoi(argv[++i]);
        } else if (!strcmp(c, "unimono") && i + 1 < argc) { M.uni_mono = (uint8_t)atoi(argv[++i]);
        } else if (!strcmp(c, "uniauto") && i + 1 < argc) { M.uni_auto = (uint8_t)atoi(argv[++i]);
        } else if (!strcmp(c, "steal") && i + 1 < argc) { M.uni_steal = (uint8_t)atoi(argv[++i]);
#ifdef SY_UNI_CAP
        } else if (!strcmp(c, "bud") && i + 1 < argc) { sy_bud = (uint8_t)atoi(argv[++i]);   /* (1.18C49: a firmware's
                                         * budget acting, synth.c sy_bud; with "maxv" its limit, "shed 1 15" its early
                                         * fades) */
#endif
        } else if (!strcmp(c, "voices")) {   /* (a line on stderr: the block, each part's voices on) */
            int k, q, keys = 0;           /* (and part A's notes held: keys with a gated voice) */
            for (q = 0; q < 128; q++) for (k = 0; k < S.nv; k++) if (S.v[k].on && S.v[k].gate && S.v[k].key == q) { keys++; break; }
            fprintf(stderr, "voices %d %d %d", sy_voices_active(&S), M.mode_now ? sy_voices_active(&SB) : 0, keys);
            for (q = 0; q < 128; q++) {   /* (then each held key of part A: key:voices) */
                int c = 0;
                for (k = 0; k < S.nv; k++) c += S.v[k].on && S.v[k].gate && S.v[k].key == q;
                if (c) fprintf(stderr, " %d:%d", q, c);
            }
            fprintf(stderr, "\n");
        } else if ((!strcmp(c, "a") || !strcmp(c, "b")) && i + 3 < argc) {
            sy_midi(c[0] == 'a' ? &S : &SB, (uint8_t)strtol(argv[i + 1], 0, 0), (uint8_t)atoi(argv[i + 2]), (uint8_t)atoi(argv[i + 3])); i += 3;
#endif
        } else { fprintf(stderr, "unknown command %s\n", c); return 2; }
    }
    if (getenv("SY_REST")) fprintf(stderr, "rest %ld\n", rest_at);   /* (tools/perf/tailtest.py) */
    fseek(out, 4, SEEK_SET); le32(out, 36 + frames * 6);
    fseek(out, 40, SEEK_SET); le32(out, frames * 6);
    fclose(out);
#if defined(HYB_SHIMMER) && defined(CLN_RVS_LOG)
    if (shcap) fclose(shcap);
    if (shlog) fclose(shlog);
#endif
    if (in) fclose(in);
    return 0;
}
