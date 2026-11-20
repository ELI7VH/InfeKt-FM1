/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 J0KERPACK */
/* waves.c (included by app.c): the user wave bank, 0.90.
 *
 * The player's own oscillator waves for slots 2-63: 64 harmonics each (amplitude a half float, phase a cycle / 65536)
 * and a name, received over MIDI SysEx (USB, Bluetooth) and kept in the flash. An empty slot plays the factory wave
 * (the engine's own design, sy_waves.c). The engine reads a slot's record when it builds a wave table (a patch load,
 * a wave changed, sy_waves_rebuild): the run-time cost is that of the factory waves. Patches keep the wave number.
 *
 * The flash: the bank does not fit the store (fm1-base's objects are 3840 B at most and the data area's object
 * places are all taken but one sector), so it lives in the platform's second window for an app's own data
 * (plat_data, PLAT_XDATA1: 0xE7000 up), as a log in two regions of UW_SECS sectors, UW_PER_SEC records of UW_REC
 * bytes a sector (bytes 0xF00..0xF07 of every sector stay erased: plat.h, the update loader's record). A region's
 * first record is its head (its generation); a write or a clear appends one record (no erase but one a sector, when
 * the log enters it); the newest record of a slot wins. A full region is compacted into the other one (the slots'
 * newest waves, then its head last: a cut-off compaction leaves the old region in charge); CLEAR ALL starts the other
 * region empty. Every record carries its region's generation and a CRC: a torn write reads as none. So uploads cost
 * one 4 KiB erase per UW_PER_SEC waves (plus a compaction's every ~40 at most), not one per wave.
 *
 * Record (UW_REC bytes): 'U' 'W', kind (UW_WAVE, UW_CLEAR, UW_HEAD), slot (2..63), the generation (4, little end),
 * the engine's record (SY_UWAVE_LEN: synth.h), CRC-16 of all that (little end), the rest erased.
 *
 * SysEx: F0 7D 56 46 cmd ... F7 (7D the non-commercial id, 56 46 "VF": this firmware's fixed tag, kept for compatibility); the wave's bytes 7-bit packed
 * (each 7 bytes as 8: first their top bits, bit k byte k's), the checksum a CRC-16 (CCITT, 0xFFFF) of the record's
 * SY_UWAVE_LEN bytes as 3 bytes of 7 bits (bits 14-15, 7-13, 0-6):
 *   01            hello      -> 41 version slots first used[9] (bit k of used[j]: slot first + 7 j + k a user wave)
 *   10 ss w.. c3  write slot -> 50 10 ss status c3 (the CRC of what the flash holds now)
 *   11 ss         clear slot -> 50 11 ss status 0 0 0
 *   12 7F         clear all  -> 50 12 00 status 0 0 0
 *   13 ss         read slot  -> 53 ss kind w.. c3 (kind 1 a user wave, 0 the factory wave's harmonics), or 50 13 ss st
 *   20-22         the boot image's (splash.c, 0.99)
 * status 0 ok, 1 checksum, 2 length, 3 flash, 4 slot, 5 command. A message that is not right changes nothing.
 * After a write or a clear the oscillator tables holding the slot are built again between audio blocks, one a block
 * (app_render: uw_render), as a patch load builds them. */
#define UW_FIRST 2u
#define UW_SLOTS 62u
#define UW_REC 288u                       /* bytes a record (UW_PER_SEC of them below 0xF00) */
#define UW_PER_SEC 13u
#define UW_SECS 8u                        /* sectors a region */
#define UW_N (UW_PER_SEC * UW_SECS)       /* records a region (the head and 103 waves) */
#define UW_BASE0 0xE7000u                 /* region 0; region 1 above it (0xEF000); PLAT_XDATA1, 0xF7000 up stays free */
#define UW_LEN (8u + SY_UWAVE_LEN + 2u)   /* bytes written of a record */
#define UW_PACKED ((SY_UWAVE_LEN / 7u) * 8u + (SY_UWAVE_LEN % 7u ? SY_UWAVE_LEN % 7u + 1u : 0u))
#define UW_VERSION 1u
enum { UW_WAVE = 1, UW_CLEAR = 2, UW_HEAD = 3 };
enum { UWS_OK, UWS_SUM, UWS_LEN, UWS_FLASH, UWS_SLOT, UWS_CMD };

static struct {
    uint32_t gen;                         /* the region in charge: its generation (0: none yet, the bank empty) */
    uint8_t reg, next;                    /* its number, its next free record */
} uw PLAT_POOL;
/* the main loop's scratch: the screen's canvas (fm1-base gfx.c cv_px, the pool; drawn into afresh by every frame:
 * nothing of it is kept between frames), taken after its last transfer (lcd_sync; uw_cmd). The pool has no room for
 * buffers of their own (the Bluetooth two-part build) */
_Static_assert(CV_MAX * 2u >= 1536u, "the canvas holds the wave bank's scratch");
#define uw_rec ((uint8_t *)(void *)cv_px)           /* a record to write (RAM: the flash's writes take it there) */
#define uw_fr ((uint8_t *)(void *)cv_px)            /* a frame out (not at once with uw_rec's record) */
#define uw_fac ((uint8_t *)(void *)cv_px + 512)     /* a factory wave's record (a dump) */
#define uw_keep ((uint8_t *)(void *)cv_px + 1024)   /* a record kept while uw_rec starts or compacts a region */

static UI_LATE uint32_t uw_addr(uint32_t reg, uint32_t k)   /* record k of region reg in the flash */
{
    return UW_BASE0 + reg * UW_SECS * 4096u + k / UW_PER_SEC * 4096u + k % UW_PER_SEC * UW_REC;
}
static UI_LATE uint32_t uw_crc(const uint8_t *p, uint32_t n)   /* CRC-16/CCITT-FALSE */
{
    uint32_t c = 0xFFFFu, k;
    while (n--) {
        c ^= (uint32_t)*p++ << 8;
        for (k = 0; k < 8u; k++) c = c & 0x8000u ? (c << 1 ^ 0x1021u) & 0xFFFFu : (c << 1) & 0xFFFFu;
    }
    return c;
}
static UI_LATE uint32_t uw_gen_of(const uint8_t *r) { return r[4] | r[5] << 8 | r[6] << 16 | (uint32_t)r[7] << 24; }
static UI_LATE int uw_ok(const uint8_t *r, uint32_t gen)   /* a whole record of this generation */
{
    return r && r[0] == 'U' && r[1] == 'W' && uw_gen_of(r) == gen &&
           uw_crc(r, UW_LEN - 2u) == (r[UW_LEN - 2u] | (uint32_t)r[UW_LEN - 1u] << 8);
}

/* each slot's newest record: 0 none, else 1 + region * UW_N + k (the audio interrupt reads it: a byte each, so a
 * slot's entry is whole whichever region it names; a compaction never erases what an entry names) */
static volatile uint8_t uw_idx[UW_SLOTS] PLAT_POOL;
/* slots whose oscillator tables are to be built again (bit s - 2): uw_new the main loop's (written), uw_mark the audio
 * interrupt's (marked stale, then built one a block); uw_seq moved: some are. (The main loop's read-modify-write of
 * uw_new may give back a bit the interrupt has just taken: the tables marked again, built once more: harmless) */
static volatile uint32_t uw_new[2] PLAT_POOL;   /* (uw_seq, uw_seen: app.c, beside patch_req) */
static uint32_t uw_mark[2] PLAT_POOL;

static const uint8_t *uw_get(int id)      /* the engine's sy_user_wave (in the audio interrupt too): id's record */
{
    uint32_t e;
    const uint8_t *r;
    if (id < (int)UW_FIRST || id >= (int)(UW_FIRST + UW_SLOTS) || !(e = uw_idx[id - (int)UW_FIRST])) return 0;
    e--;
    r = plat_data(uw_addr(e / UW_N, e % UW_N));
    return r ? r + 8 : 0;
}

static void uw_render(void)               /* audio interrupt, a block's start (uw_seq moved): the tables holding a slot
                                          * written marked, then one of them built again a block */
{
    uint32_t k;
    const uint8_t q = uw_seq;
    for (k = 0; k < UW_SLOTS; k++)
        if (uw_new[k >> 5] >> (k & 31u) & 1u) {
            sy_waves_stale(&S, (int)(k + UW_FIRST));
#ifdef HYB_MULTI
            sy_waves_stale(&SB, (int)(k + UW_FIRST));
#endif
            uw_new[k >> 5] &= ~(1u << (k & 31u));
            uw_mark[k >> 5] |= 1u << (k & 31u);
        }
    for (k = 0; k < UW_SLOTS; k++)
        if (uw_mark[k >> 5] >> (k & 31u) & 1u) {
            if (sy_waves_rebuild(&S, (int)(k + UW_FIRST))) return;
#ifdef HYB_MULTI
            if (sy_waves_rebuild(&SB, (int)(k + UW_FIRST))) return;
#endif
            uw_mark[k >> 5] &= ~(1u << (k & 31u));   /* (all built) */
        }
    uw_seen = q;
}
static UI_LATE void uw_touch(uint32_t slot) /* slot's tables to build again */
{
    const uint32_t k = slot - UW_FIRST;
    uw_new[k >> 5] |= 1u << (k & 31u);
    uw_seq = (uint8_t)(uw_seq + 1u);
}

static UI_LATE void uw_scan(void)         /* app_init: the region in charge, its records, each slot's newest */
{
    uint32_t r, k, best = 0, g;
    const uint8_t *h;
    uw.gen = 0;
    for (k = 0; k < UW_SLOTS; k++) uw_idx[k] = 0;
    uw_new[0] = uw_new[1] = uw_mark[0] = uw_mark[1] = 0;
    uw_seen = uw_seq;
    for (r = 0; r < 2u; r++) {
        h = plat_data(uw_addr(r, 0));
        if (!h || h[2] != UW_HEAD) continue;
        g = uw_gen_of(h);
        if (!uw_ok(h, g) || !g) continue;
        if (!uw.gen || (int32_t)(g - best) > 0) { best = g; uw.gen = g; uw.reg = (uint8_t)r; }
    }
    if (!uw.gen) return;
    for (k = 1; k < UW_N; k++) {
        const uint8_t *p = plat_data(uw_addr(uw.reg, k));
        if (!p) break;
        if (k % UW_PER_SEC == 0u && uw_gen_of(p) != uw.gen) break;   /* (a sector not erased for this region yet) */
        if (p[0] == 0xFFu && p[1] == 0xFFu && p[2] == 0xFFu && p[3] == 0xFFu && uw_gen_of(p) == 0xFFFFFFFFu) break;
        if (!uw_ok(p, uw.gen) || p[3] < UW_FIRST || p[3] >= UW_FIRST + UW_SLOTS) continue;   /* (torn: none) */
        if (p[2] == UW_WAVE) uw_idx[p[3] - UW_FIRST] = (uint8_t)(1u + uw.reg * UW_N + k);
        else if (p[2] == UW_CLEAR) uw_idx[p[3] - UW_FIRST] = 0;
    }
    uw.next = (uint8_t)k;
}

static UI_LATE int uw_put(uint32_t reg, uint32_t k, uint32_t kind, uint32_t slot, uint32_t gen)   /* uw_rec's record
                                                  * (its engine's record filled) as record k of region reg: 0 ok */
{
    const uint32_t a = uw_addr(reg, k);
    uint32_t c, i;
    const uint8_t *p;
    uw_rec[0] = 'U'; uw_rec[1] = 'W'; uw_rec[2] = (uint8_t)kind; uw_rec[3] = (uint8_t)slot;
    for (i = 0; i < 4u; i++) uw_rec[4 + i] = (uint8_t)(gen >> (8u * i));
    c = uw_crc(uw_rec, UW_LEN - 2u);
    uw_rec[UW_LEN - 2u] = (uint8_t)c; uw_rec[UW_LEN - 1u] = (uint8_t)(c >> 8);
    if (k % UW_PER_SEC == 0u && plat_data_erase(a) != 0) return -1;   /* (a sector's first: erased for it) */
    if (plat_data_write(a, uw_rec, UW_LEN) != 0) return -1;
    p = plat_data(a);
    for (i = 0; i < UW_LEN; i++)
        if (!p || p[i] != uw_rec[i]) return -1;
    return 0;
}
static UI_LATE int uw_head(uint32_t reg, uint32_t gen)   /* region reg started empty, generation gen: 0 ok */
{
    uint32_t i;
    for (i = 8; i < UW_LEN; i++) uw_rec[i] = 0;
    if (uw_put(reg, 0, UW_HEAD, 0, gen) != 0) return -1;
    return 0;
}
static UI_LATE int uw_compact(void)       /* the other region from the slots' newest waves; then in charge: 0 ok */
{
    const uint32_t r = uw.reg ^ 1u, g = uw.gen + 1u;
    uint32_t s, k = 1, i;
    uint8_t nidx[UW_SLOTS];
    if (plat_data_erase(uw_addr(r, 0)) != 0) return -1;   /* (the head's sector first: an old head is gone) */
    for (s = 0; s < UW_SLOTS; s++) {
        const uint8_t *src = uw_get((int)(s + UW_FIRST));
        nidx[s] = 0;
        if (!src) continue;
        for (i = 0; i < SY_UWAVE_LEN; i++) uw_rec[8 + i] = src[i];
        if (uw_put(r, k, UW_WAVE, s + UW_FIRST, g) != 0) return -1;
        nidx[s] = (uint8_t)(1u + r * UW_N + k);
        k++;
    }
    for (i = 8; i < UW_LEN; i++) uw_rec[i] = 0;
    uw_rec[0] = 'U'; uw_rec[1] = 'W'; uw_rec[2] = UW_HEAD; uw_rec[3] = 0;
    for (i = 0; i < 4u; i++) uw_rec[4 + i] = (uint8_t)(g >> (8u * i));
    {   /* the head last (its sector erased above: no erase again) */
        const uint32_t c = uw_crc(uw_rec, UW_LEN - 2u);
        uw_rec[UW_LEN - 2u] = (uint8_t)c; uw_rec[UW_LEN - 1u] = (uint8_t)(c >> 8);
        if (plat_data_write(uw_addr(r, 0), uw_rec, UW_LEN) != 0 || !uw_ok(plat_data(uw_addr(r, 0)), g)) return -1;
    }
    uw.reg = (uint8_t)r; uw.gen = g; uw.next = (uint8_t)k;
    for (s = 0; s < UW_SLOTS; s++) uw_idx[s] = nidx[s];
    return 0;
}
static UI_LATE int uw_append(uint32_t kind, uint32_t slot)   /* uw_rec's engine record (UW_WAVE) or a clear into the
                                                  * log: 0 ok (slot's entry set) */
{
    if (!uw.gen || uw.next >= UW_N) {     /* (the bank's first write: region 0 started; full: compacted. The record
                                           * waits meanwhile, uw_rec being theirs) */
        uint32_t i;
        int rc;
        for (i = 0; i < SY_UWAVE_LEN; i++) uw_keep[i] = uw_rec[8 + i];
        if (!uw.gen) {
            rc = uw_head(0, 1);
            if (!rc) { uw.gen = 1; uw.reg = 0; uw.next = 1; }
        } else
            rc = uw_compact();
        for (i = 0; i < SY_UWAVE_LEN; i++) uw_rec[8 + i] = uw_keep[i];
        if (rc) return -1;
    }
    if (uw_put(uw.reg, uw.next, kind, slot, uw.gen) != 0) { uw.next++; return -1; }   /* (a bad record: skipped) */
    uw_idx[slot - UW_FIRST] = kind == UW_WAVE ? (uint8_t)(1u + uw.reg * UW_N + uw.next) : 0u;
    uw.next++;
    return 0;
}
static UI_LATE int uw_clear_all(void)     /* the other region started empty, in charge: 0 ok */
{
    const uint32_t r = uw.gen ? uw.reg ^ 1u : 0u, g = uw.gen + 1u;
    uint32_t s;
    if (uw_head(r, g) != 0) return -1;
    uw.reg = (uint8_t)r; uw.gen = g; uw.next = 1;
    for (s = 0; s < UW_SLOTS; s++)
        if (uw_idx[s]) { uw_idx[s] = 0; uw_touch(s + UW_FIRST); }
    return 0;
}

/* ---- SysEx */
static UI_LATE uint32_t uw_pack(const uint8_t *in, uint32_t n, uint8_t *out)   /* 7 bytes -> 8: bytes out */
{
    uint32_t i, o = 0;
    for (i = 0; i < n; i += 7u) {
        uint32_t k, m = n - i < 7u ? n - i : 7u, hi = 0;
        for (k = 0; k < m; k++) hi |= (uint32_t)(in[i + k] >> 7) << k;
        out[o++] = (uint8_t)hi;
        for (k = 0; k < m; k++) out[o++] = in[i + k] & 0x7Fu;
    }
    return o;
}
static UI_LATE int uw_unpack(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t want)   /* 0: exactly want bytes */
{
    uint32_t i = 0, o = 0;
    while (i < n && o < want) {
        const uint32_t hi = in[i++];
        uint32_t k;
        for (k = 0; k < 7u && i < n && o < want; k++) out[o++] = (uint8_t)(in[i++] | (hi >> k & 1u) << 7);
    }
    return o == want && i == n ? 0 : -1;
}
static UI_LATE uint32_t uw_hdr(uint32_t cmd)   /* the frame's head: F0 7D 56 46 cmd */
{
    uw_fr[0] = 0xF0; uw_fr[1] = 0x7D; uw_fr[2] = 0x56; uw_fr[3] = 0x46; uw_fr[4] = (uint8_t)cmd;
    return 5u;
}
static UI_LATE uint32_t uw_sum(uint32_t n, uint32_t c)   /* the CRC's 3 bytes and F7 after n bytes: the length */
{
    uw_fr[n++] = (uint8_t)(c >> 14 & 3u); uw_fr[n++] = (uint8_t)(c >> 7 & 0x7Fu); uw_fr[n++] = (uint8_t)(c & 0x7Fu);
    uw_fr[n++] = 0xF7;
    return n;
}
static UI_LATE void uw_ack(uint32_t cmd, uint32_t slot, uint32_t st, uint32_t c)   /* 50 cmd slot status c3 */
{
    const uint8_t f[12] = {0xF0, 0x7D, 0x56, 0x46, 0x50, (uint8_t)(cmd & 0x7Fu), (uint8_t)(slot & 0x7Fu), (uint8_t)st,
                           (uint8_t)(c >> 14 & 3u), (uint8_t)(c >> 7 & 0x7Fu), (uint8_t)(c & 0x7Fu), 0xF7};
    plat_sysex_send(f, sizeof f);
}
static UI_LATE void uw_name(char *d, const uint8_t *r)   /* the record's name, trimmed */
{
    uint32_t k;
    for (k = 0; k < SY_UW_NAMES; k++) {
        const char c = (char)r[k];
        d[k] = c >= 32 && c < 127 ? c : ' ';
    }
    while (k && d[k - 1u] == ' ') k--;
    d[k] = 0;
}
static UI_LATE void uw_say(uint32_t slot, const char *what, const uint8_t *r)   /* the strip: "WAVE 12 LOADED", the
                                                  * name */
{
    char a[24], b[SY_UW_NAMES + 1u];
    a[0] = b[0] = 0;
    text_cat(a, "WAVE ");
    bu_num(a, slot);
    text_cat(a, what);
    if (r) uw_name(b, r);
    app_say(a, b);
    ui.msg_until = plat_ms() + 2500u;
}
static UI_LATE void uw_factory(uint32_t slot, uint8_t *r)   /* the factory wave's record (a dump): its harmonics, the
                                                  * largest 1, the trim that gives its own level */
{
    float amp[65], phs[65], mx = 0.f, pk = 0.f;
    uint32_t n, i;
    sy_waves_harm((int)slot, amp, phs);
    for (n = 1; n <= 64u; n++) if (amp[n] > mx) mx = amp[n];
    for (i = 0; i < 256u; i++) {         /* (its peak at the engine's level: the trim) */
        float y = 0.f;
        for (n = 1; n <= 64u; n++) y += amp[n] * sy_sin2pi((float)n * (float)i * (1.f / 256.f) + phs[n]);
        if (sy_absf(y) > pk) pk = sy_absf(y);
    }
    for (n = 0; n < SY_UW_NAMES; n++) r[n] = (uint8_t)"FACTORY   "[n];
    {   /* (the table's peak: the trim) */
        const float db = 20.f * 0.30103f * sy_log2(pk > 1e-6f ? pk : 1e-6f);
        r[SY_UW_TRIM] = (uint8_t)(int8_t)(db >= 0.f ? (int)(db * 10.f + 0.5f) : -(int)(-db * 10.f + 0.5f));
    }
    r[SY_UW_TRIM + 1] = 0;
    for (n = 1; n <= 64u; n++) {
        float a = amp[n] / (mx > 0.f ? mx : 1.f), p = phs[n] - (float)sy_floori(phs[n]);
        uint32_t h = 0, e = 15u, q;
        if (a > 0.f) {                     /* (a float in [0, 1] as a half) */
            while (a < 1.f && e) { a *= 2.f; e--; }
            if (e) q = e << 10 | ((uint32_t)((a - 1.f) * 1024.f + 0.5f) & 1023u);
            else q = (uint32_t)(a * 512.f + 0.5f);   /* (subnormal) */
            h = q;
        }
        q = (uint32_t)(p * 65536.f + 0.5f) & 0xFFFFu;
        r[SY_UW_HARM + 4u * (n - 1u)] = (uint8_t)h; r[SY_UW_HARM + 4u * (n - 1u) + 1u] = (uint8_t)(h >> 8);
        r[SY_UW_HARM + 4u * (n - 1u) + 2u] = (uint8_t)q; r[SY_UW_HARM + 4u * (n - 1u) + 3u] = (uint8_t)(q >> 8);
    }
}
static UI_LATE int uw_slot_ok(uint32_t s) { return s >= UW_FIRST && s < UW_FIRST + UW_SLOTS; }

static UI_LATE void uw_cmd(const uint8_t *a, uint32_t n)   /* a: cmd .. (after 7D 56 46), n its bytes */
{
    const uint32_t cmd = a[0], slot = n > 1u ? a[1] : 0u;
    uint32_t k;
    lcd_sync();                           /* (the canvas is the scratch: its last transfer done) */
    if (cmd == 0x01u) {                   /* hello: the version, the slots, which are the player's */
        uint32_t m = uw_hdr(0x41);
        uw_fr[m++] = UW_VERSION; uw_fr[m++] = UW_SLOTS; uw_fr[m++] = UW_FIRST;
        for (k = 0; k < 9u; k++) uw_fr[m + k] = 0;
        for (k = 0; k < UW_SLOTS; k++)
            if (uw_idx[k]) uw_fr[m + k / 7u] |= (uint8_t)(1u << (k % 7u));
        m += 9u;
        uw_fr[m++] = 0xF7;
        plat_sysex_send(uw_fr, m);
    } else if (cmd == 0x10u) {            /* write */
        uint32_t c, st = UWS_OK;
        if (!uw_slot_ok(slot)) st = UWS_SLOT;
        else if (n != 2u + UW_PACKED + 3u || uw_unpack(a + 2, UW_PACKED, uw_rec + 8, SY_UWAVE_LEN) != 0) st = UWS_LEN;
        else if (((uint32_t)a[n - 3u] << 14 | (uint32_t)a[n - 2u] << 7 | a[n - 1u]) != uw_crc(uw_rec + 8, SY_UWAVE_LEN))
            st = UWS_SUM;
        else if (uw_append(UW_WAVE, slot) != 0) st = UWS_FLASH;
        if (st) { uw_ack(cmd, slot, st, 0); if (st == UWS_FLASH) uw_say(slot, " FAILED", 0); return; }
        {
            const uint8_t *r = uw_get((int)slot);
            c = r ? uw_crc(r, SY_UWAVE_LEN) : 0u;
            uw_touch(slot);
            uw_ack(cmd, slot, UWS_OK, c);
            uw_say(slot, " LOADED", uw_rec + 8);
        }
    } else if (cmd == 0x11u) {            /* clear a slot */
        uint32_t st = UWS_OK;
        if (n != 2u || !uw_slot_ok(slot)) st = n != 2u ? UWS_LEN : UWS_SLOT;
        else if (uw_idx[slot - UW_FIRST]) {   /* (empty already: nothing written) */
            for (k = 8; k < UW_LEN; k++) uw_rec[k] = 0;
            if (uw_append(UW_CLEAR, slot) != 0) st = UWS_FLASH;
            else uw_touch(slot);
        }
        uw_ack(cmd, slot, st, 0);
        if (!st) uw_say(slot, " FACTORY", 0);
    } else if (cmd == 0x12u) {            /* clear all (7F: no stray byte does it) */
        const uint32_t st = n != 2u || slot != 0x7Fu ? UWS_LEN : uw_clear_all() != 0 ? UWS_FLASH : UWS_OK;
        uw_ack(cmd, 0, st, 0);
        if (!st) app_say("WAVES 2-63", "FACTORY");
    } else if (cmd == 0x13u) {            /* read a slot back */
        const uint8_t *r;
        uint32_t m;
        if (n != 2u || !uw_slot_ok(slot)) { uw_ack(cmd, slot, n != 2u ? UWS_LEN : UWS_SLOT, 0); return; }
        r = uw_get((int)slot);
        if (!r) { uw_factory(slot, uw_fac); r = uw_fac; }
        m = uw_hdr(0x53);
        uw_fr[m++] = (uint8_t)slot;
        uw_fr[m++] = (uint8_t)(r != uw_fac);
        m += uw_pack(r, SY_UWAVE_LEN, uw_fr + m);
        plat_sysex_send(uw_fr, uw_sum(m, uw_crc(r, SY_UWAVE_LEN)));
    } else if ((cmd & 0xF0u) == 0x20u)   /* the boot image's (splash.c) */
        bi_cmd(a, n);
    else
        uw_ack(cmd & 0x7Fu, slot, UWS_CMD, 0);
}
