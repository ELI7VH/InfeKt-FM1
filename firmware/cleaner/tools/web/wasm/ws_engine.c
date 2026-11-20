/* SPDX-License-Identifier: GPL-3.0-only */
/* The web synth: the cleaner's engine (cleaner/engine, the same sources as the firmware) as a freestanding
 * WebAssembly module for the InfeKt page's "sound in the browser". Built by build.sh with the
 * Bluetooth two-part firmware's engine switches (HYB_MULTI, 16 voice slots, the 32768-frame delay, the unison caps,
 * REVERB on the lent float ring, so FX > SHIMMER is there as on a device booted with Bluetooth off). Part A alone
 * (SINGLE), as the device as shipped.
 *
 * The page holds the patch as the editor does (the "panel form": 256 bytes and the shimmer's extension, the shimmer
 * targets as 200 + t in the matrix / LFO assign bytes); this wrapper turns it into what the engine takes, as the
 * firmware's app does (app.c shx_eb, shx_sync, edit_set, the patch change in app_render).
 *
 * Exports (all ints; the buffers are in the module's memory, read through the pointers):
 *   ws_init(hz)               the engine at hz (the device: 44100); the setup as shipped (ws_setup below)
 *   ws_patch()                -> the panel-form buffer (WS_PLEN bytes) the page writes before ws_load
 *   ws_load()                 that buffer as the patch: notes off, loaded at the next block (as a patch change)
 *   ws_set(b, v)              one byte of the panel form as an edit (queued: the engine's next block, its latency)
 *   ws_midi(st, d1, d2)       a MIDI message (note on/off, CC, pitch bend, ...) through the part setup
 *   ws_all_off()              every voice off now (PANIC)
 *   ws_setup(maxv, uni, unim, uauto, steal, dyn)   GLO > AUDIO's voice settings (app.c mx_defaults: 14 4 6 0 1 1,
 *                             SINGLE's UNI MAX 4 since 1.18C48; the page plays one part)
 *   ws_rvq(full)              REVERB: 0 HALF (as shipped), 1 FULL
 *   ws_osc(clean, flt_sm)     GLO > AUDIO > OSC (0 ORIGINAL, 1 CLEAN) and FILT KNOBS (0 ORIGINAL, 1 SMOOTH)
 *   ws_opt(o)                 the engine's option flags (synth.h SY_OPT_*; SY_OPT_LATENCY as on the device)
 *   ws_render()               one block (WS_BLOCK frames) into ws_out_l / ws_out_r; returns the voices sounding
 *   ws_out_l(), ws_out_r()    -> the block's float outputs (full scale 1.0, clipped as the device's)
 *   ws_block(), ws_plen()     WS_BLOCK, WS_PLEN
 *   ws_uwave_buf(), ws_uwave_set(id)   (1.18C47w4) a user wave as the firmware's bank holds one:
 *                             the page writes a record (SY_UWAVE_LEN bytes) into the buffer, then sets it as wave id
 *                             (2..63); the tables holding that wave are built again. None set: the factory waves.
 * Nothing allocates; the module imports nothing. */
#include "synth.h"

#define WS_EXPORT(n) __attribute__((export_name(#n)))
#define WS_PLEN (256 + HYB_SHIM_N + 1)   /* (1.18C47: W GLIDE past the shimmer's values, app.c SHX_GL) */

static sy_synth S, SB;
/* the shared memory as the firmware's Bluetooth-off map (and the lab's HYB_RV_LEND build): the shimmer's state right
 * below the lent ring, the lent ring's lower 64 KB right below SH.sp (its upper half) */
static struct { float pre[HYB_RV_LEND_PRE / 4u + 1u]; float lo[16384]; sy_shared_t sh; } SHL;
static float CH_A[2][2048], CH_B[2][2048], SAT_B[512 + 1];
static sy_multi_t M;
static float out_l[SY_BLOCK], out_r[SY_BLOCK];

/* ---- the panel form (app.c, the shimmer's extension 0.89 / 1.18C45) ---- */
#define SHX_NR 12
static const uint8_t SHX_RB[SHX_NR] = {128 + 65, 128 + 68, 128 + 70, 128 + 73, 128 + 75, 128 + 77,
                                       128 + 104, 128 + 107, 128 + 110, 128 + 79, 128 + 81, 128 + 11};
#define SHX_VD 200
#define SHX_NT 2
#define SHX_L3OFF 3                     /* (LFO 3's byte off the shimmer: app.c SHX_L3OFF) */
static const uint8_t SHX_DEF[HYB_SHIM_N + 1] = {0, 0, 100, 64, 0, 0, 0, 57, 5};   /* (app.c SHX_DEFS, SHX_GDEF) */
static uint8_t pbuf[WS_PLEN];
static uint8_t shx[HYB_SHIM_N + SHX_NR];
static uint8_t shx_dirty, load_req;

static int shx_ri(int b)
{
    int k;
    for (k = 0; k < SHX_NR; k++)
        if (SHX_RB[k] == b) return k;
    return -1;
}
static int eng_byte(int b, int v)       /* panel byte b's value v as the engine takes it (app.c shx_eb, the queue's clamp) */
{
    if (v >= SHX_VD && shx_ri(b) >= 0) return b == 128 + 11 ? SHX_L3OFF : 0;
    return v < 0 ? 0 : v > 127 ? 127 : v;
}
static void shx_sync(void)              /* app.c shx_sync: the extension and the targets, taken at the next block */
{
    int k;
    for (k = 0; k < HYB_SHIM_N; k++) shx[k] = pbuf[256 + k];
    shx[6] = (uint8_t)((pbuf[256 + 6] & 1) | ((pbuf[256 + HYB_SHIM_N] & 15) ^ 5) << 1);   /* (app.c shx_tl: TAIL, W GLIDE) */
    for (k = 0; k < SHX_NR; k++) {
        const int v = pbuf[SHX_RB[k]];
        shx[HYB_SHIM_N + k] = (uint8_t)(v >= SHX_VD && v < SHX_VD + SHX_NT ? v - SHX_VD + 1 : 0);
    }
    shx_dirty = 1;
}

WS_EXPORT(ws_setup) void ws_setup(int maxv, int uni, int unim, int uauto, int steal, int dyn)
{
    M.max_voices = (int16_t)(maxv < 1 ? 1 : maxv > SY_MAX_VOICES ? SY_MAX_VOICES : maxv);
    S.max_voices = M.max_voices;
    M.uni_max = (uint8_t)uni;
    M.uni_mono = (uint8_t)unim;
    M.uni_auto = (uint8_t)uauto;
    M.uni_steal = (uint8_t)steal;
    M.dyn = (uint8_t)dyn;
}
WS_EXPORT(ws_init) void ws_init(int hz)
{
    int k;
    sy_multi_init(&M, &S, &SB, &SHL.sh, CH_A, CH_B, SAT_B, (float)hz);
    hyb_rv_lend(SHL.lo);                /* (REVERB FULL's float ring and the shimmer: Bluetooth's memory lent) */
    S.channel = -1;                     /* any MIDI channel (app.c app_init) */
    ws_setup(14, 4, 6, 0, 1, 1);        /* GLO > AUDIO as shipped (app.c mx_defaults, the Bluetooth build's LATENCY HIGH;
                                         * 1.18C48: UNI MAX SINGLE 4, MONO UNI MAX 6; until 1.18C47 2 and 5) */
    hyb_rv_set_full(0);                 /* REVERB as shipped in the Bluetooth two-part build: HALF (app.c RVQ_DEF_Q) */
    for (k = 0; k < WS_PLEN; k++) pbuf[k] = k < 256 ? 0 : SHX_DEF[k - 256];
    shx_sync();
}
/* the lab's state for the fidelity check (tools/web/wasm/fidelity.mjs against host/lab_engine.c built alike):
 * sy_multi_init's setup as it leaves it, the channel the lab sets, the reverb as sy_init leaves it */
WS_EXPORT(ws_init_lab) void ws_init_lab(int hz)
{
    int k;
    sy_multi_init(&M, &S, &SB, &SHL.sh, CH_A, CH_B, SAT_B, (float)hz);
    hyb_rv_lend(SHL.lo);
    S.channel = 0;
    for (k = 0; k < WS_PLEN; k++) pbuf[k] = k < 256 ? 0 : SHX_DEF[k - 256];
    shx_dirty = 0;
}
WS_EXPORT(ws_patch) uint8_t *ws_patch(void) { return pbuf; }
WS_EXPORT(ws_load) void ws_load(void) { load_req = 1; shx_sync(); }
/* the lab's "preset": the 256 bytes as they are, at once (no extension, no translation) */
WS_EXPORT(ws_load_raw) void ws_load_raw(void) { sy_load_patch(&S, pbuf); }
WS_EXPORT(ws_shim_raw) void ws_shim_raw(void) { hyb_rv_shim(pbuf + 256); }   /* (the lab's SY_SHIM) */
WS_EXPORT(ws_set) void ws_set(int b, int v)
{
    if (b < 0 || b >= WS_PLEN || v < 0 || v > 255) return;
    pbuf[b] = (uint8_t)v;
    if (b >= 256 || shx_ri(b) >= 0) shx_sync();
    if (b < 240 || (b >= 250 && b < 256))   /* (the name, 240..249: the panel's alone) */
        sy_midi(&S, SY_EV_PARAM, (uint8_t)b, (uint8_t)eng_byte(b, v));
}
WS_EXPORT(ws_midi) void ws_midi(int st, int d1, int d2) { sy_multi_midi(&M, (uint8_t)st, (uint8_t)d1, (uint8_t)d2); }
WS_EXPORT(ws_all_off) void ws_all_off(void) { sy_all_off(&S); sy_all_off(&SB); }
WS_EXPORT(ws_rvq) void ws_rvq(int full) { hyb_rv_set_full(full); }
WS_EXPORT(ws_osc) void ws_osc(int clean, int flt_sm)
{
    S.osc_clean = SB.osc_clean = clean ? 1.f : 0.f;
    S.flt_sm = SB.flt_sm = (uint8_t)(flt_sm ? 1 : 0);
}
WS_EXPORT(ws_opt) void ws_opt(int o) { S.opt = SB.opt = (uint32_t)o; }
WS_EXPORT(ws_get_opt) int ws_get_opt(void) { return (int)S.opt; }
WS_EXPORT(ws_render) int ws_render(void)
{
    if (load_req) {                     /* a patch change between two blocks (app.c app_render) */
        uint8_t p[256];
        int k;
        for (k = 0; k < 256; k++) p[k] = (uint8_t)eng_byte(k, pbuf[k]);
        sy_all_off(&S);
        sy_load_patch(&S, p);
        load_req = 0;
    }
    if (shx_dirty) {                    /* FX > SHIMMER: part A's values to the reverb */
        int k;
        hyb_rv_shim(shx);
        for (k = 0; k < SHX_NR; k++) S.shx_rt[k] = shx[HYB_SHIM_N + k];
        shx_dirty = 0;
    }
    sy_multi_render(&M, out_l, out_r, 0, 0);
    return sy_multi_voices_active(&M);
}
WS_EXPORT(ws_out_l) float *ws_out_l(void) { return out_l; }
WS_EXPORT(ws_out_r) float *ws_out_r(void) { return out_r; }
WS_EXPORT(ws_block) int ws_block(void) { return SY_BLOCK; }
WS_EXPORT(ws_plen) int ws_plen(void) { return WS_PLEN; }
WS_EXPORT(ws_voices) int ws_voices(void) { return sy_multi_voices_active(&M); }

/* ---- (1.18C47w4) the user wave bank, as the firmware's (app.c uw_get): the page's waves (Sete's, make_page.py --waves) */
static uint8_t uw_buf[SY_UWAVE_LEN], uw_rec[64][SY_UWAVE_LEN], uw_on[64];
static const uint8_t *ws_uw_get(int id) { return id >= 2 && id < 64 && uw_on[id] ? uw_rec[id] : 0; }
WS_EXPORT(ws_uwave_buf) uint8_t *ws_uwave_buf(void) { return uw_buf; }
WS_EXPORT(ws_uwave_set) void ws_uwave_set(int id)
{
    if (id < 2 || id > 63) return;
    __builtin_memcpy(uw_rec[id], uw_buf, SY_UWAVE_LEN);
    uw_on[id] = 1;
    sy_user_wave = ws_uw_get;
    sy_waves_stale(&S, id); sy_waves_stale(&SB, id);
    while (sy_waves_rebuild(&S, id)) { }
    while (sy_waves_rebuild(&SB, id)) { }
}
