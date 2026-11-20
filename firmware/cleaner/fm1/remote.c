/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 J0KERPACK */
/* ------------------------------------------------ the remote editor (0.98): SysEx F0 7D 52 45 --- */
/* the development notes; included by app.c before app_sysex (UI_LATE, the main loop alone). The InfeKt page's
 * Editor tab edits the panel's edit buffer (shadow: the part the panel edits, its 256 bytes and the shimmer's
 * extension) over USB, as the panel does: each byte through edit_set (the undo step, its coalescing, the edit memory
 * and its mark, the engine's queue), nothing written to the flash before SAVE. It hears back what changes on the
 * device: the buffer is looked at every RE_SCAN_MS (a hash a chunk of RE_CH bytes, the patch's state), and only while
 * an editor has said hello in the last RE_ALIVE_MS; without one nothing is sent at all.
 *   page -> FM-1: 01 hello (and its keepalive, at most RE_ALIVE_MS apart); 02 dump; 03 seq part (ih il vh vl)*n
 *                 set (n <= RE_SETN); 04 bye; 05 p the part edited (two parts); 06 UNDO
 *   FM-1 -> page: 41 ver flags len_h len_l STAT (hello; flags 1 two parts, 2 the shimmer, 4 4B);
 *                 42 STAT len_h len_l PACK(the buffer) (dump);
 *                 43 seq status (set: 0 taken, 1 not the part edited, 2 a byte refused); 45 STAT (part);
 *                 46 STAT (undo);
 *                 48 STAT (the patch's state changed); 49 part k PACK(chunk k) (a chunk changed);
 *                 4A part (i v)*n (MIDI CC: the engine's page A bytes that differ from the panel's; not in the buffer:
 *                 the panel does not follow a controller either);
 *                 4B part l1..l9 (the matrix's slots live: each slot's modulation now, |source x amount| in parameter
 *                 steps 0..127, 0 for a slot not in use; at most every RE_SCAN_MS, only when it changed, only when
 *                 nothing else went out in that pass and USB's queue has room: re_lv)
 *   (1.18C27, the two-part builds: the pattern launcher's sets, app.c ls_*; the page's export
 *   and import. hello's flags 8: there are sets)
 *   page -> FM-1: 07 k read slot k (0 .. 15; 7F: the format); 08 k PACK(record) write slot k; 09 k empty slot k
 *   FM-1 -> page: 47 k st [rec_h rec_l PACK(the record) tagA(4) tagB(4)] (st 0 a set, 1 empty, 2 no such slot; the
 *                 patches' tags "C012" as the panel shows them); 47 7F 0 slots version rec_h rec_l (the format);
 *                 4C k st (write) / 4D k st (empty): 0 done, 1 not a set of this format, 2 no such slot, 3 the
 *                 transport runs (STOP FIRST: no flash write while it plays), 4 the flash failed, 5 the panel's own
 *                 write waits (SAVE PENDING). A write is the page's explicit import (its user's click), as SAVE
 *   STAT: part, state (1 edited, 2 a SysEx single, 4 part B can be edited), patch_h patch_l, its tag "A012" (4)
 *   PACK(n bytes): their low 7 bits, then their high bits, 7 a byte (bit i of byte j: byte 7 j + i's bit 7) */
#define RE_VER 1u
#define RE_ALIVE_MS 5000u                /* an editor's frame this recent: it is there */
#define RE_SCAN_MS 40u                   /* the buffer looked at this often (while one is there) */
#define RE_CH 32u
#define RE_NCH ((SH_LEN + RE_CH - 1u) / RE_CH)
#define RE_SETN 16u                      /* (a set frame's bytes at most: the page's) */
#define RE_ROOM 32u                      /* the matrix's activity sent only with this much room in USB's out queue
                                          * (half: a host that reads; the lowest priority, never in an upload's way) */
#ifndef HYB_REMOTE_LV                    /* the matrix's slots live (4B): not in the Bluetooth single build (its flash
                                          * is short: ~600 B); its editor's lights show the slots in use alone there */
#if FM1B_BLE && !defined(HYB_MULTI)
#define HYB_REMOTE_LV 0
#else
#define HYB_REMOTE_LV 1
#endif
#endif
#define re_fr ((uint8_t *)(void *)cv_px) /* a frame out: the canvas, as the wave bank's (waves.c; lcd_sync first) */
static struct { uint32_t t, scan, st, lv, h[RE_NCH]; } re PLAT_POOL;   /* the editor's last frame (0: none), the last
                                          * look, the state's and the chunks' hashes as the editor has them */
#ifdef HYB_MULTI
#define RE_PART ed_part
#else
#define RE_PART 0u
#endif

static UI_LATE uint32_t re_hash(uint32_t k)   /* chunk k's hash */
{
    return fnv(shadow + k * RE_CH, k + 1u < RE_NCH ? RE_CH : SH_LEN - k * RE_CH);
}
static UI_LATE uint32_t re_hdr(uint8_t *f, uint32_t cmd)   /* F0 7D 52 45 cmd: 5 */
{
    f[0] = 0xF0; f[1] = 0x7D; f[2] = 0x52; f[3] = 0x45; f[4] = (uint8_t)cmd;
    return 5u;
}
static UI_LATE uint32_t re_stat(uint8_t *f)   /* STAT: 8 */
{
    f[0] = (uint8_t)RE_PART;
    f[1] = (uint8_t)((em_edited(set.patch) ? 1u : 0u) | (sx_edit ? 2u : 0u)
#ifdef HYB_MULTI
                     | (set.mx_mode != SY_M_SINGLE ? 4u : 0u)
#endif
                     );
    f[2] = (uint8_t)(set.patch >> 7); f[3] = (uint8_t)(set.patch & 0x7Fu);
    patch_tag((char *)f + 4, set.patch);   /* (its 0 at f[8]: the next byte's) */
    return 8u;
}
static UI_LATE uint32_t re_pack(uint8_t *f, const uint8_t *s, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < (n + 6u) / 7u; k++) f[n + k] = 0;
    for (k = 0; k < n; k++) {
        f[k] = s[k] & 0x7Fu;
        f[n + k / 7u] |= (uint8_t)(s[k] >> 7 << (k % 7u));
    }
    return n + (n + 6u) / 7u;
}
static UI_LATE void re_out(uint32_t n)   /* the frame of n bytes and its F7 out */
{
    re_fr[n] = 0xF7;
    plat_sysex_send(re_fr, n + 1u);
}
static UI_LATE void re_say(uint32_t cmd)   /* cmd STAT */
{
    re_out(re_hdr(re_fr, cmd) + re_stat(re_fr + 5));
}
static UI_LATE void re_dump(void)        /* 42: the whole buffer (the editor has it as it is now) */
{
    uint8_t *f = re_fr;
    uint32_t k, m = re_hdr(f, 0x42);
    m += re_stat(f + m);
    re.st = fnv(f + 5, 8u);
    f[m++] = (uint8_t)(SH_LEN >> 7); f[m++] = (uint8_t)(SH_LEN & 0x7Fu);
    m += re_pack(f + m, shadow, SH_LEN);
    for (k = 0; k < RE_NCH; k++) re.h[k] = re_hash(k);
    re_out(m);
}
static UI_LATE int re_row_has(int32_t r, uint32_t by)   /* row r (U_ROW, -1 none) shows byte by */
{
    uint32_t k;
    if (r >= 0)
        for (k = 0; k < 4u; k++) {
            const u_param_t *p = &U_ROW[r].p[k];
            if (p->label[0] && (P_BYTE0(p) == by || (p->kind == V_WPW && p->byte == by + 1u))) return 1;
        }
    return 0;
}
static UI_LATE int re_set(uint32_t by, uint32_t v, uint32_t now)   /* byte by to v as the panel's edit; 1: refused */
{
    const u_param_t *p = param_of(by);
    if (by >= SH_LEN) return 1;
#ifdef HYB_SHIMMER
    if (by >= 256u && !SHX_LIVE) return 1;   /* (1.04: the shimmer's with Bluetooth on, as its knobs: ui.c P_BTOFF) */
    if (v > 127u && !(shx_ri(by) >= 0 && v >= SHX_VD && v < SHX_VD + SHX_NT)) return 1;   /* (a shimmer target) */
#else
    if (v > 127u) return 1;
#endif
    st_busy_t = now;                     /* (as a knob: AUTO SAVE waits) */
    if (v == shadow[by]) return 0;
    edit_set(by, v, now);
    if (p) { ui.lastp = p; ui.edit_t = now | 1u; ui.d_lcd = 1; }   /* (the strip: its name and value, as a knob's) */
    if (by == 17u || by == 22u || by == 112u || ui.vl_until || ui.view == V_PLAY || re_row_has(lit.it, by)
        || re_row_has(lit.dim, by))
        ui.d_rows = 1;                   /* (the rows only when they show it, or their labels follow it) */
    if (by >= 240u && by < 250u) home_sig[0] = home_sig[1] = 0;   /* (the name: HOME's rows) */
    return 0;
}
#ifdef HYB_MULTI
static UI_LATE void re_sets(const uint8_t *a, uint32_t n)   /* 07, 08, 09: the launcher's sets */
{
    uint8_t *f = re_fr;
    const uint32_t c = a[0], k = n >= 2u ? a[1] : 0x7Fu, pn = LS_REC + (LS_REC + 6u) / 7u;
    uint32_t m = re_hdr(f, c == 0x07u ? 0x47u : c == 0x08u ? 0x4Cu : 0x4Du), st = 0, i;
    f[m++] = (uint8_t)(k & 0x7Fu);
    if (c == 0x07u && k == 0x7Fu) {      /* (the format) */
        f[m++] = 0; f[m++] = LS_SLOTS; f[m++] = LS_V2; f[m++] = (uint8_t)(LS_REC >> 7); f[m++] = (uint8_t)(LS_REC & 0x7Fu);
        re_out(m);
        return;
    }
    if (k >= LS_SLOTS) st = 2;
    else if (c == 0x07u) {
        ls_rec_t r;
        if (!ls_read(k, &r)) st = 1;
        else {
            f[m++] = 0;
            f[m++] = (uint8_t)(LS_REC >> 7); f[m++] = (uint8_t)(LS_REC & 0x7Fu);
            m += re_pack(f + m, (const uint8_t *)&r, LS_REC);
            for (i = 0; i < 2u; i++) {
                const uint32_t p = (uint32_t)r.pt[i][0] | (uint32_t)r.pt[i][1] << 8;
                patch_tag((char *)f + m, p < PATCH_N ? p : PATCH_INIT);
                m += 4u;
            }
            re_out(m);
            return;
        }
    } else if (sp_runs()) st = 3;
    else if (lset.pend) st = 5;
    else if (c == 0x08u) {
        uint8_t *b = (uint8_t *)&ls_buf;
        if (n != 2u + pn) st = 1;
        else {
            for (i = 0; i < LS_REC; i++) b[i] = (uint8_t)((a[2u + i] & 0x7Fu) | (a[2u + LS_REC + i / 7u] >> (i % 7u) & 1u) << 7);
            if (!ls_v1(&ls_buf) || ln_rows_bad(&ls_buf.ln[0][0])) st = 1;   /* (a set exported by 1.18C29: as
                                          * this one's, LS_V2) */
            else if (ls_put(k)) st = 4;
        }
    } else {
        for (i = 0; i < LS_REC; i++) ((uint8_t *)&ls_buf)[i] = 0;
        if ((ls_full() >> k & 1u) && ls_put(k)) st = 4;
    }
    f[m++] = (uint8_t)st;
    re_out(m);
}
#endif
static UI_LATE void re_cmd(const uint8_t *a, uint32_t n)   /* a: cmd .. (after 7D 52 45) */
{
    uint8_t *f = re_fr;
    const uint32_t c = a[0], now = plat_ms();
    uint32_t k, st = 0;
    const int was = re.t && (int32_t)(now - re.t) <= (int32_t)RE_ALIVE_MS;   /* (an editor there already: a hello is its
                                          * keepalive; signed: re.t is now | 1, 1.18C46) */
    lcd_sync();                          /* (the canvas is the frame) */
    re.t = now | 1u;
    if (c == 0x04u) { re.t = 0; return; }
    if (c == 0x01u) {                    /* hello: the version, the build, the buffer's length, the state */
        k = re_hdr(f, 0x41);
        f[k++] = RE_VER;
        f[k++] = (uint8_t)(
#ifdef HYB_MULTI
            1u | 8u |                    /* (8: the launcher's sets, 07 .. 09) */
#endif
#ifdef HYB_SHIMMER
            (SHX_LIVE ? 2u : 0u) |       /* (1.04: none with Bluetooth on in the Bluetooth two-part build) */
#endif
            (HYB_REMOTE_LV ? 4u : 0u));
        f[k++] = (uint8_t)(SH_LEN >> 7); f[k++] = (uint8_t)(SH_LEN & 0x7Fu);
        re_out(k + re_stat(f + k));
        if (!was) {                      /* (a new editor: nothing to tell until it asks for the dump; a keepalive's
                                          * hello leaves what is not yet told to be told) */
            for (k = 0; k < RE_NCH; k++) re.h[k] = re_hash(k);
            re.st = fnv(f + 9, 8u);
        }
        return;
    }
    if (c == 0x02u) { re_dump(); return; }
    if (c == 0x03u && n >= 3u) {         /* set: seq part, then the bytes */
        if (a[2] != RE_PART) st = 1;
        else
            for (k = 3; k + 3u < n; k += 4u) {
                const uint32_t by = (uint32_t)a[k] << 7 | a[k + 1u], i = by / RE_CH;
                const int clean = i < RE_NCH && re_hash(i) == re.h[i];   /* (the editor has the chunk: it knows the
                                          * change, no echo; a change of the panel's not yet told: told) */
                if (re_set(by, (uint32_t)a[k + 2u] << 7 | a[k + 3u], now)) st = 2;
                if (clean) re.h[i] = re_hash(i);
            }
        k = re_hdr(f, 0x43);
        f[k++] = a[1] & 0x7Fu;
        f[k++] = (uint8_t)st;
        re_out(k);
        return;
    }
#ifdef HYB_MULTI
    if (c == 0x05u && n >= 2u) { part_edit(a[1] & 1u); re_say(0x45); return; }
#endif
    if (c == 0x06u) { undo_step(); re_say(0x46); return; }
#ifdef HYB_MULTI
    if (c >= 0x07u && c <= 0x09u) { re_sets(a, n); return; }
#endif
}
#if HYB_REMOTE_LV
#ifdef HYB_MULTI
#define RE_NV(s) ((s)->nv)
#else
#define RE_NV(s) SY_MAX_VOICES
#endif
/* the matrix's slots as they act now (4B): the panel's slot (shadow: source, destination, amount; in use as its light
 * says, ui.c mx_slot) times its source's value as the engine's control computes it (synth.c src_value, read here, the
 * engine untouched): the bend, pressure, the controllers (smoothed), and for the voices' sources the voice started last
 * that still sounds (the highest age; fx_take's choice for the effects; none sounding: 0), its envelopes' levels, its
 * LFOs' outputs (a mono LFO: the part's), velocity, key. The value is |source x (amount - 64) x 2|, parameter steps,
 * 127 at most: the slot's offset on its destination, as matrix() adds it (before the destination's limits) */
static const uint8_t RE_SRC_CC[16] = {1, 2, 3, 4, 6, 8, 9, 11, 12, 13, 14, 15, 16, 64, 65, 66};   /* (synth.c SRC_CC) */
static UI_LATE uint32_t re_lv(uint8_t *lv)   /* the nine levels into lv; their hash */
{
#ifdef HYB_MULTI
    const sy_synth *s = ed_part ? &SB : &S;
#else
    const sy_synth *s = &S;
#endif
    const sy_voice_t *v = 0;
    uint32_t i;
    for (i = 0; i < (uint32_t)RE_NV(s); i++)
        if (s->v[i].on && (!v || s->v[i].age > v->age)) v = &s->v[i];
    for (i = 0; i < MX_N; i++) {
        const u_param_t *p = U_ROW[U_NROW - MX_N + i].p;
        const uint32_t src = shadow[p[0].byte], dst = shadow[p[1].byte], amt = shadow[p[2].byte];
        float x = 0.f;
        if (!src || !dst || amt == 64u) { lv[i] = 0; continue; }
        if (src == 1u) x = s->bend;
        else if (src == 2u) x = s->pressure;
        else if (src <= 18u) {
            const float c = s->ccs[RE_SRC_CC[src - 3u]];
            x = c >= 127.f ? 1.f : c * (1.f / 128.f);
        }
        else if (src >= 27u) x = 0.5f;
        else if (v) {
            static const uint8_t MODE[3] = {70u, 82u, 128u + 9u};   /* (the LFOs' mode bytes: mono, the part's LFO) */
            if (src == 19u) x = v->aenv.lvl;
            else if (src == 20u) x = v->fenv.lvl;
            else if (src <= 23u)
                x = 0.5f * (s->patch[MODE[src - 21u]] ? s->glfo[src - 21u].out : v->lfo[src - 21u].out);
            else if (src <= 25u) x = (float)v->vel * (1.f / 127.f);
            else x = ((float)v->key - 60.f) * (1.f / 64.f);
        }
        x *= ((float)amt - 64.f) * 2.f;
        if (x < 0.f) x = -x;
        lv[i] = (uint8_t)(x >= 127.f ? 127u : (uint32_t)(x + 0.5f));
    }
    return fnv(lv, MX_N);
}
#else
#define re_lv(lv) (re.lv)                /* (none: never changes) */
#endif
static UI_LATE void re_tick(uint32_t now)   /* app_frame: what changed told, an editor there */
{
    uint8_t *f = re_fr, st[8], lv[MX_N];
    uint32_t k, m, n = 0, lh;
    int said = 0;
    if (!re.t || now - re.scan < RE_SCAN_MS) return;
    if ((int32_t)(now - re.t) > (int32_t)RE_ALIVE_MS) { re.t = 0; return; }   /* (gone quiet: nothing more; signed,
                                          * 1.18C46: re.t is now | 1, a frame in the same millisecond as the
                                          * editor's message saw 0xFFFFFFFF and let it go) */
    re.scan = now;
    for (k = 0; k < RE_NCH; k++) n += re_hash(k) != re.h[k];
    re_stat(st);
    lh = re_lv(lv);
    if (HYB_REMOTE_LV && lh != re.lv && (bu.on || plat_midi_out_room(TO_USB) < RE_ROOM))
        lh = re.lv;                      /* (the activity waits: an upload, a queue that is not read) */
    if (!n && !re_cc && fnv(st, 8u) == re.st && lh == re.lv) return;   /* (nothing new: the canvas left alone) */
    lcd_sync();                          /* (the canvas is the frame) */
    if (n > 2u) { re_dump(); re_cc = 0; return; }   /* (a patch, a part, an undo of a patch: the whole buffer) */
    if (fnv(st, 8u) != re.st) { re.st = fnv(st, 8u); re_say(0x48); said = 1; }
    for (k = 0; n && k < RE_NCH; k++)
        if (re_hash(k) != re.h[k]) {
            m = re_hdr(f, 0x49);
            f[m++] = (uint8_t)RE_PART;
            f[m++] = (uint8_t)k;
            m += re_pack(f + m, shadow + k * RE_CH, k + 1u < RE_NCH ? RE_CH : SH_LEN - k * RE_CH);
            re.h[k] = re_hash(k);
            re_out(m);
            said = 1;
        }
    if (re_cc) {                         /* MIDI CC: the engine's page A where it differs from the panel's */
#ifdef HYB_MULTI
        const uint8_t *e = ed_part ? SB.patch : S.patch;
#else
        const uint8_t *e = S.patch;
#endif
        re_cc = 0;
        m = re_hdr(f, 0x4A);
        f[m++] = (uint8_t)RE_PART;
        for (k = 0; k < 120u; k++)       /* (not the mod wheel, the part's volume: the engine's own, nor the pedal) */
            if (k != 1u && k != 7u && k != 64u && e[k] != shadow[k] && m < 6u + 2u * 40u) {
                f[m++] = (uint8_t)k;
                f[m++] = e[k] & 0x7Fu;
            }
        if (m > 6u) { re_out(m); said = 1; }
    }
    if (HYB_REMOTE_LV && lh != re.lv && !said) {   /* the matrix's activity: last, alone in its pass */
        re.lv = lh;
        m = re_hdr(f, 0x4B);
        f[m++] = (uint8_t)RE_PART;
        for (k = 0; k < MX_N; k++) f[m++] = lv[k];
        re_out(m);
    }
}
